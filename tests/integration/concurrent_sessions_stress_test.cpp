// SPDX-License-Identifier: MIT
//
// Concurrent-sessions stress test, run primarily under ThreadSanitizer.
//
// 10 client threads each open a FIX session against one TcpServer and send
// 1000 NewOrderSingle messages with interleaved buy/sell. We assert that
// every client received an ExecutionReport per submitted order and that
// the server's `messages_in` counter matches.
//
// Under -fsanitize=thread, this is the regression net for the shared
// matcher/book accesses behind `book_mu_`. Without TSan, it still catches
// deadlock or stalls. GTEST_REPEAT iterates the test for variance.

#include "fix/messages.h"
#include "fix/parser.h"
#include "fix/serializer.h"
#include "net/tcp_server.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
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

bool send_all(int fd, const std::string& s) {
    const char* p = s.data();
    std::size_t left = s.size();
    while (left > 0) {
        ssize_t w = ::send(fd, p, left, 0);
        if (w <= 0) return false;
        p += w;
        left -= static_cast<std::size_t>(w);
    }
    return true;
}

bool wait_readable(int fd, int ms) {
    pollfd pf{fd, POLLIN, 0};
    int pr = ::poll(&pf, 1, ms);
    return pr > 0 && (pf.revents & POLLIN);
}

}  // namespace

// Lower message count and session count when run under sanitizer builds
// to keep CI under 5 minutes. With OBFIX_STRESS_FULL=1, run the full
// 10x1000 grid; default is 10x200 so the standard CI build still exercises
// the contention path without timing out the GH runner's TSan job.
TEST(ConcurrentSessions, TenSessionsKMessagesEach) {
    const bool full = std::getenv("OBFIX_STRESS_FULL") != nullptr;
    const int n_sessions = 10;
    const int n_msgs_per = full ? 1000 : 200;

    net::ServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.sender_comp_id = "OBFIX";
    cfg.algo = MatchAlgo::Fifo;
    net::TcpServer srv(cfg);
    ASSERT_TRUE(srv.start());
    auto port = srv.bound_port();
    ASSERT_NE(port, 0);

    std::atomic<int> ok_count{0};
    std::atomic<int> err_count{0};

    auto client = [&](int id) {
        int fd = connect_loopback(port);
        if (fd < 0) {
            err_count.fetch_add(1);
            return;
        }
        Serializer ser(kSohWire);
        Parser psr(kSohWire);
        std::string sender = "C" + std::to_string(id);

        // Logon
        auto logon = ser.build({{tag::MsgType, msgtype::Logon},
                                {tag::SenderCompID, sender},
                                {tag::TargetCompID, "OBFIX"},
                                {tag::MsgSeqNum, "1"},
                                {tag::SendingTime, "20260513-00:00:00.000"},
                                {108, "30"}});
        if (!send_all(fd, logon)) {
            ::close(fd);
            err_count.fetch_add(1);
            return;
        }
        std::string buf;
        char tmp[8192];
        // Wait for logon echo (one full FIX message).
        while (true) {
            if (!wait_readable(fd, 2000)) {
                ::close(fd);
                err_count.fetch_add(1);
                return;
            }
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) {
                ::close(fd);
                err_count.fetch_add(1);
                return;
            }
            buf.append(tmp, static_cast<std::size_t>(n));
            auto r = psr.parse_one(buf);
            if (r.err == ParseError::None) {
                buf.erase(0, r.consumed);
                break;
            }
            if (r.err != ParseError::Incomplete) {
                ::close(fd);
                err_count.fetch_add(1);
                return;
            }
        }

        int seen_acks = 0;
        for (int i = 0; i < n_msgs_per; ++i) {
            char side = (i % 2 == 0) ? '1' : '2';
            std::int64_t px = 10000 + ((id + i) % 11) - 5;
            auto msg = ser.build({{tag::MsgType, msgtype::NewOrderSingle},
                                  {tag::SenderCompID, sender},
                                  {tag::TargetCompID, "OBFIX"},
                                  {tag::MsgSeqNum, std::to_string(2 + i)},
                                  {tag::SendingTime, "20260513-00:00:00.000"},
                                  {tag::ClOrdID, sender + "-" + std::to_string(i)},
                                  {tag::Symbol, "SYM"},
                                  {tag::Side, std::string(1, side)},
                                  {tag::OrderQty, "100"},
                                  {tag::Price, std::to_string(px)},
                                  {tag::OrdType, "2"},
                                  {tag::TransactTime, "20260513-00:00:00.000"}});
            if (!send_all(fd, msg)) {
                ::close(fd);
                err_count.fetch_add(1);
                return;
            }
            // Drain at least one ExecutionReport (the New ack).
            bool got_ack = false;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!got_ack && std::chrono::steady_clock::now() < deadline) {
                bool progress = true;
                while (progress) {
                    progress = false;
                    auto r = psr.parse_one(buf);
                    if (r.err != ParseError::None) break;
                    if (r.msg.msg_type() == "8" && r.msg.get(tag::ExecType).value_or("") == "0") {
                        got_ack = true;
                        ++seen_acks;
                    }
                    buf.erase(0, r.consumed);
                    progress = true;
                    if (got_ack) break;
                }
                if (got_ack) break;
                if (!wait_readable(fd, 100)) continue;
                ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
                if (n <= 0) {
                    ::close(fd);
                    err_count.fetch_add(1);
                    return;
                }
                buf.append(tmp, static_cast<std::size_t>(n));
            }
            if (!got_ack) {
                ::close(fd);
                err_count.fetch_add(1);
                return;
            }
        }

        ::close(fd);
        if (seen_acks == n_msgs_per) ok_count.fetch_add(1);
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(n_sessions));
    for (int i = 0; i < n_sessions; ++i) threads.emplace_back(client, i + 1);
    for (auto& t : threads) t.join();

    EXPECT_EQ(ok_count.load(), n_sessions);
    EXPECT_EQ(err_count.load(), 0);
    // Server must have observed every NewOrderSingle.
    EXPECT_GE(srv.messages_in(), static_cast<std::uint64_t>(n_sessions * n_msgs_per));
    srv.stop();
}
