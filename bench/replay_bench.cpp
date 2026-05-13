// SPDX-License-Identifier: MIT
// End-to-end FIX session bench. Spins a TcpServer on loopback, drives N
// NewOrderSingle messages through it, and measures wall-clock latency
// from "wrote FIX bytes" to "received the first ExecutionReport bytes".
//
// The session generates a variable number of ExecutionReports per inbound
// order (1 Ack + 0..K Trades). To avoid socket buffer back-pressure, the
// client drains all readable bytes between iterations and reads with a
// non-blocking deadline.
//
// Output: JSON to stdout (or a path given by --out). The numbers in
// bench/results/bench_local.json were produced by this harness on the
// hardware noted in README.md.

#include "fix/parser.h"
#include "fix/serializer.h"
#include "fix/messages.h"
#include "net/tcp_server.h"
#include "obs/histogram.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace obfix;
using namespace obfix::fix;

namespace {

int connect_loopback(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int big = 1 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const char* p, std::size_t n) {
    while (n > 0) {
        ssize_t w = ::send(fd, p, n, 0);
        if (w <= 0) return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

// Drain whatever the kernel has buffered without blocking. Appends to buf.
// Returns false on socket error.
bool drain_nonblocking(int fd, std::string& buf) {
    char tmp[16384];
    while (true) {
        pollfd pf{fd, POLLIN, 0};
        int pr = ::poll(&pf, 1, 0);
        if (pr <= 0) return true;
        if (!(pf.revents & POLLIN)) return true;
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK;
        if (n == 0) return false;  // peer closed
        buf.append(tmp, static_cast<std::size_t>(n));
    }
}

// Block until at least one ExecutionReport is parseable out of `buf` or
// `deadline_ms` passes. Returns true on success and pops the report.
bool wait_for_exec_report(int fd, std::string& buf, Parser& p, int deadline_ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(deadline_ms);
    char tmp[16384];
    while (true) {
        // Try parse what we have.
        while (!buf.empty()) {
            auto r = p.parse_one(buf);
            if (r.err == ParseError::Incomplete) break;
            if (r.err != ParseError::None) {
                buf.clear();
                return false;
            }
            bool is_er = r.msg.msg_type() == "8";
            buf.erase(0, r.consumed);
            if (is_er) return true;
        }
        auto now = std::chrono::steady_clock::now();
        if (now >= end) return false;
        int ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - now).count());
        pollfd pf{fd, POLLIN, 0};
        int pr = ::poll(&pf, 1, ms);
        if (pr <= 0) return false;
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, static_cast<std::size_t>(n));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n_msgs = 100000;
    std::string out_path;
    MatchAlgo algo = MatchAlgo::ProRata;
    bool smoke = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--n" && i + 1 < argc)
            n_msgs = static_cast<std::size_t>(std::stoull(argv[++i]));
        else if (a == "--out" && i + 1 < argc)
            out_path = argv[++i];
        else if (a == "--algo" && i + 1 < argc) {
            std::string v = argv[++i];
            algo = (v == "fifo") ? MatchAlgo::Fifo : MatchAlgo::ProRata;
        } else if (a == "--smoke") {
            n_msgs = 1000;
            smoke = true;
        }
    }

    net::ServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.sender_comp_id = "OBFIX";
    cfg.algo = algo;
    net::TcpServer srv(cfg);
    if (!srv.start()) {
        std::fprintf(stderr, "server start failed\n");
        return 1;
    }
    auto port = srv.bound_port();

    int fd = connect_loopback(port);
    if (fd < 0) {
        srv.stop();
        return 1;
    }

    Serializer ser(kSohWire);
    Parser psr(kSohWire);

    auto build = [&](SeqNum seq, const std::string& clord, char side, std::int64_t px,
                     std::uint64_t qty) {
        return ser.build({
            {tag::MsgType, msgtype::NewOrderSingle},
            {tag::SenderCompID, "CLIENT"},
            {tag::TargetCompID, "OBFIX"},
            {tag::MsgSeqNum, std::to_string(seq)},
            {tag::SendingTime, "20260513-00:00:00.000"},
            {tag::ClOrdID, clord},
            {tag::Symbol, "SYM"},
            {tag::Side, std::string(1, side)},
            {tag::OrderQty, std::to_string(qty)},
            {tag::Price, std::to_string(px)},
            {tag::OrdType, "2"},
            {tag::TransactTime, "20260513-00:00:00.000"},
        });
    };

    // Logon and drain the echo.
    auto logon = ser.build({{tag::MsgType, msgtype::Logon},
                            {tag::SenderCompID, "CLIENT"},
                            {tag::TargetCompID, "OBFIX"},
                            {tag::MsgSeqNum, "1"},
                            {tag::SendingTime, "20260513-00:00:00.000"},
                            {108, "30"}});
    send_all(fd, logon.data(), logon.size());
    std::string buf;
    // Wait until we see the logon echo back (one full FIX message).
    while (true) {
        if (!drain_nonblocking(fd, buf)) break;
        auto r = psr.parse_one(buf);
        if (r.err == ParseError::None) {
            buf.erase(0, r.consumed);
            break;
        }
        pollfd pf{fd, POLLIN, 0};
        ::poll(&pf, 1, 100);
    }

    // Warmup: 1000 messages not counted.
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> px_off(-5, 5);
    auto run_block = [&](std::size_t n, std::size_t seq_base, bool measure,
                         obs::LogHistogram& hist) {
        for (std::size_t i = 0; i < n; ++i) {
            char side = (i % 2 == 0) ? '1' : '2';
            std::int64_t px = 10000 + px_off(rng);
            auto msg = build(seq_base + i, "o-" + std::to_string(seq_base + i), side, px, 100);
            auto s0 = std::chrono::steady_clock::now();
            if (!send_all(fd, msg.data(), msg.size())) return false;
            if (!wait_for_exec_report(fd, buf, psr, 5000)) return false;
            auto s1 = std::chrono::steady_clock::now();
            if (measure) {
                auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(s1 - s0).count();
                hist.record(static_cast<std::uint64_t>(ns));
            }
            // Drain any pending reports from previous iterations so the
            // server's send buffer never fills up.
            if (!drain_nonblocking(fd, buf)) return false;
        }
        return true;
    };

    obs::LogHistogram warm;
    obs::LogHistogram hist;
    if (!smoke) {
        // 2000-msg warmup to amortize first-touch costs (page faults, TLB).
        if (!run_block(2000, 2, false, warm)) {
            std::fprintf(stderr, "warmup failed\n");
            ::close(fd);
            srv.stop();
            return 1;
        }
    }
    auto t0 = std::chrono::steady_clock::now();
    if (!run_block(n_msgs, smoke ? 2 : 2002, true, hist)) {
        std::fprintf(stderr, "measure block failed\n");
        ::close(fd);
        srv.stop();
        return 1;
    }
    auto t1 = std::chrono::steady_clock::now();
    auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    double secs = static_cast<double>(total_ns) / 1e9;
    double mps = static_cast<double>(n_msgs) / secs;

    ::close(fd);
    srv.stop();

    auto p50 = hist.quantile(0.50);
    auto p95 = hist.quantile(0.95);
    auto p99 = hist.quantile(0.99);
    auto p999 = hist.quantile(0.999);
    std::uint64_t avg = hist.count() ? hist.sum() / hist.count() : 0;

    char outbuf[2048];
    std::snprintf(outbuf, sizeof(outbuf),
                  "{\n"
                  "  \"n_messages\": %zu,\n"
                  "  \"algo\": \"%s\",\n"
                  "  \"wall_seconds\": %.6f,\n"
                  "  \"msgs_per_sec\": %.2f,\n"
                  "  \"avg_ns\": %llu,\n"
                  "  \"p50_ns\": %llu,\n"
                  "  \"p95_ns\": %llu,\n"
                  "  \"p99_ns\": %llu,\n"
                  "  \"p999_ns\": %llu,\n"
                  "  \"smoke\": %s\n"
                  "}\n",
                  n_msgs, (algo == MatchAlgo::ProRata ? "prorata" : "fifo"), secs, mps,
                  static_cast<unsigned long long>(avg), static_cast<unsigned long long>(p50),
                  static_cast<unsigned long long>(p95), static_cast<unsigned long long>(p99),
                  static_cast<unsigned long long>(p999), smoke ? "true" : "false");
    if (out_path.empty()) {
        std::fputs(outbuf, stdout);
    } else {
        std::ofstream f(out_path);
        f << outbuf;
        std::fputs(outbuf, stdout);
    }
    return 0;
}
