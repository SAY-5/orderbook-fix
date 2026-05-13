// SPDX-License-Identifier: MIT
#include "tcp_server.h"

#include "../obs/logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <utility>

namespace obfix::net {

namespace {
using namespace obfix::fix;

// Translate a Trade/Ack into an ExecutionReport body (without 35= which
// the session inserts).
std::vector<std::pair<int, std::string>> exec_report_for_ack(const Ack& a, OrderId id) {
    return {
        {tag::OrderID, std::to_string(id)},
        {tag::ClOrdID, a.clord_id.value},
        {tag::ExecID, std::to_string(id) + "-A"},
        {tag::ExecType, "0"},  // New
        {tag::OrdStatus, "0"},
        {tag::LeavesQty, "0"},  // overwritten below if we know it
        {tag::CumQty, "0"},
        {tag::AvgPx, "0"},
    };
}

std::vector<std::pair<int, std::string>> exec_report_for_trade(const Trade& t, OrderId resting_id,
                                                               OrderId aggressor_id,
                                                               bool for_aggressor) {
    OrderId oid = for_aggressor ? aggressor_id : resting_id;
    const ClOrdID& cl = for_aggressor ? t.aggressor_clord : t.resting_clord;
    return {
        {tag::OrderID, std::to_string(oid)},
        {tag::ClOrdID, cl.value},
        {tag::ExecID, std::to_string(oid) + "-T" + std::to_string(t.qty)},
        {tag::ExecType, "F"},   // Trade
        {tag::OrdStatus, "1"},  // PartiallyFilled (session does not track final state)
        {tag::LastQty, std::to_string(t.qty)},
        {tag::LastPx, std::to_string(t.price)},
    };
}

}  // namespace

TcpServer::TcpServer(ServerConfig cfg) : cfg_(std::move(cfg)), book_("SYM"), matcher_(cfg_.algo) {}

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg_.port);
    inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        obs::log_warn("tcp", std::string("bind failed: ") + std::strerror(errno));
        ::close(fd);
        return false;
    }
    socklen_t len = sizeof(addr);
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    bound_port_.store(ntohs(addr.sin_port));
    if (::listen(fd, 32) < 0) {
        ::close(fd);
        return false;
    }
    // Publish listen_fd_ before the acceptor thread starts. The atomic
    // store happens-before the thread create call, which the thread sees
    // when it loads listen_fd_ on its first iteration.
    listen_fd_.store(fd);
    running_.store(true);
    acceptor_ = std::thread(&TcpServer::accept_loop, this);
    return true;
}

void TcpServer::stop() {
    if (!running_.exchange(false)) return;
    int fd = listen_fd_.exchange(-1);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    if (acceptor_.joinable()) acceptor_.join();
    std::lock_guard<std::mutex> g(sessions_mu_);
    for (auto& t : session_threads_)
        if (t.joinable()) t.join();
    session_threads_.clear();
}

void TcpServer::accept_loop() {
    while (running_.load()) {
        int fd = listen_fd_.load();
        if (fd < 0) return;
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int cli = ::accept(fd, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cli < 0) {
            if (!running_.load()) return;
            continue;
        }
        int one = 1;
        ::setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        std::lock_guard<std::mutex> g(sessions_mu_);
        session_threads_.emplace_back([this, cli] { session_loop(cli); });
    }
}

void TcpServer::session_loop(int fd) {
    SessionConfig sc;
    sc.sender_comp_id = cfg_.sender_comp_id;
    sc.target_comp_id = "";  // adopted from peer Logon
    sc.heartbeat_secs = cfg_.heartbeat_secs;
    Session sess(sc, kSohWire);

    auto write_all = [&](const std::string& s) -> bool {
        const char* p = s.data();
        std::size_t left = s.size();
        while (left > 0) {
            ssize_t w = ::send(fd, p, left, 0);
            if (w <= 0) return false;
            p += w;
            left -= w;
        }
        return true;
    };

    char buf[8192];
    while (running_.load()) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        SessionStep step = sess.on_bytes(std::string_view(buf, static_cast<std::size_t>(n)));
        for (const auto& w : step.out) {
            if (!write_all(w.bytes)) {
                step.disconnect = true;
                break;
            }
            msgs_out_.fetch_add(1);
        }
        // Handle application messages: route to matcher.
        for (auto& app : step.app) {
            msgs_in_.fetch_add(1);
            const auto& m = app.msg;
            const std::string mt = m.msg_type();
            EventList events;
            {
                std::lock_guard<std::mutex> g(book_mu_);
                if (mt == msgtype::NewOrderSingle) {
                    auto o = std::make_unique<Order>();
                    o->id = next_order_id_.fetch_add(1);
                    o->clord_id = ClOrdID{m.get(tag::ClOrdID).value_or("")};
                    o->side = static_cast<Side>(m.get(tag::Side).value_or("1")[0]);
                    o->type = OrdType::Limit;
                    o->price = std::stoll(m.get(tag::Price).value_or("0"));
                    o->orig_qty = std::stoull(m.get(tag::OrderQty).value_or("0"));
                    o->leaves_qty = o->orig_qty;
                    matcher_.submit(book_, std::move(o), events);
                } else if (mt == msgtype::OrderCancelRequest) {
                    ClOrdID orig{m.get(tag::OrigClOrdID).value_or("")};
                    auto res = book_.cancel(0, orig);
                    if (res.ok) {
                        CancelAck ca{};
                        ca.id = 0;
                        ca.clord_id = ClOrdID{m.get(tag::ClOrdID).value_or("")};
                        ca.orig_clord_id = orig;
                        events.emplace_back(ca);
                    } else {
                        OrdReject r{};
                        r.clord_id = ClOrdID{m.get(tag::ClOrdID).value_or("")};
                        r.reason = "cancel: unknown order";
                        events.emplace_back(r);
                    }
                }
            }
            // Convert events to ExecutionReports.
            for (const auto& ev : events) {
                std::vector<std::pair<int, std::string>> body;
                if (std::holds_alternative<Ack>(ev)) {
                    const auto& a = std::get<Ack>(ev);
                    body = exec_report_for_ack(a, a.id);
                } else if (std::holds_alternative<Trade>(ev)) {
                    const auto& t = std::get<Trade>(ev);
                    body = exec_report_for_trade(t, t.resting_id, t.aggressor_id, true);
                } else if (std::holds_alternative<OrdReject>(ev)) {
                    const auto& r = std::get<OrdReject>(ev);
                    body = {
                        {tag::ClOrdID, r.clord_id.value},
                        {tag::ExecType, "8"},
                        {tag::OrdStatus, "8"},
                        {tag::Text, r.reason},
                    };
                } else if (std::holds_alternative<CancelAck>(ev)) {
                    const auto& c = std::get<CancelAck>(ev);
                    body = {
                        {tag::ClOrdID, c.clord_id.value},
                        {tag::OrigClOrdID, c.orig_clord_id.value},
                        {tag::ExecType, "4"},
                        {tag::OrdStatus, "4"},
                    };
                } else {
                    continue;
                }
                WireOut wo = sess.send_execution_report(body);
                if (!write_all(wo.bytes)) {
                    step.disconnect = true;
                    break;
                }
                msgs_out_.fetch_add(1);
            }
        }
        if (step.disconnect) break;
    }
    ::close(fd);
}

}  // namespace obfix::net
