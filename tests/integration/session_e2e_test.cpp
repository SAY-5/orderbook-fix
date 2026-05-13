// SPDX-License-Identifier: MIT
#include "net/tcp_server.h"
#include "fix/parser.h"
#include "fix/serializer.h"
#include "fix/messages.h"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace obfix;
using namespace obfix::fix;

namespace {

int connect_loopback(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
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
        left -= w;
    }
    return true;
}

std::string recv_until(int fd, std::size_t at_least, std::chrono::milliseconds timeout) {
    std::string buf;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    char tmp[4096];
    while (buf.size() < at_least && std::chrono::steady_clock::now() < deadline) {
        timeval tv{0, 100000};
        fd_set rfd;
        FD_ZERO(&rfd);
        FD_SET(fd, &rfd);
        int sel = ::select(fd + 1, &rfd, nullptr, nullptr, &tv);
        if (sel <= 0) continue;
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<std::size_t>(n));
    }
    return buf;
}

}  // namespace

TEST(SessionE2E, LogonNewOrderTradeLogout) {
    net::ServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.sender_comp_id = "OBFIX";
    cfg.algo = MatchAlgo::Fifo;  // deterministic
    net::TcpServer srv(cfg);
    ASSERT_TRUE(srv.start());
    auto port = srv.bound_port();
    ASSERT_NE(port, 0);

    int fd = connect_loopback(port);
    ASSERT_GE(fd, 0);

    Serializer s(kSohWire);
    Parser p(kSohWire);

    // Logon
    auto logon = s.build({{tag::MsgType, msgtype::Logon},
                          {tag::SenderCompID, "CLIENT"},
                          {tag::TargetCompID, "OBFIX"},
                          {tag::MsgSeqNum, "1"},
                          {tag::SendingTime, "20260513-00:00:00.000"},
                          {108, "30"}});
    ASSERT_TRUE(send_all(fd, logon));

    // Wait for logon echo
    std::string buf = recv_until(fd, 1, std::chrono::milliseconds(2000));
    ASSERT_FALSE(buf.empty());
    auto pr = p.parse_one(buf);
    ASSERT_EQ(pr.err, ParseError::None);
    EXPECT_EQ(pr.msg.msg_type(), "A");
    buf.erase(0, pr.consumed);

    // Resting sell at 10000 qty 100
    auto sell = s.build({{tag::MsgType, msgtype::NewOrderSingle},
                         {tag::SenderCompID, "CLIENT"},
                         {tag::TargetCompID, "OBFIX"},
                         {tag::MsgSeqNum, "2"},
                         {tag::SendingTime, "20260513-00:00:00.001"},
                         {tag::ClOrdID, "ord-1"},
                         {tag::Symbol, "SYM"},
                         {tag::Side, "2"},
                         {tag::OrderQty, "100"},
                         {tag::Price, "10000"},
                         {tag::OrdType, "2"},
                         {tag::TransactTime, "20260513-00:00:00.001"}});
    ASSERT_TRUE(send_all(fd, sell));

    // Expect an ack ExecutionReport
    buf += recv_until(fd, 1, std::chrono::milliseconds(2000));
    auto pr2 = p.parse_one(buf);
    ASSERT_EQ(pr2.err, ParseError::None) << "got bytes: " << buf.size();
    EXPECT_EQ(pr2.msg.msg_type(), "8");
    EXPECT_EQ(pr2.msg.get(tag::ExecType).value_or(""), "0");
    buf.erase(0, pr2.consumed);

    // Aggressing buy at 10000 qty 100 -> trade
    auto buy = s.build({{tag::MsgType, msgtype::NewOrderSingle},
                        {tag::SenderCompID, "CLIENT"},
                        {tag::TargetCompID, "OBFIX"},
                        {tag::MsgSeqNum, "3"},
                        {tag::SendingTime, "20260513-00:00:00.002"},
                        {tag::ClOrdID, "ord-2"},
                        {tag::Symbol, "SYM"},
                        {tag::Side, "1"},
                        {tag::OrderQty, "100"},
                        {tag::Price, "10000"},
                        {tag::OrdType, "2"},
                        {tag::TransactTime, "20260513-00:00:00.002"}});
    ASSERT_TRUE(send_all(fd, buy));

    // Expect ack + trade (two ExecutionReports). Drain until we have
    // parsed at least one ExecutionReport with ExecType=F or the timeout
    // elapses.
    int trades = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (trades == 0 && std::chrono::steady_clock::now() < deadline) {
        buf += recv_until(fd, 1, std::chrono::milliseconds(200));
        bool progress = true;
        while (progress) {
            progress = false;
            auto r = p.parse_one(buf);
            if (r.err != ParseError::None || r.consumed == 0) break;
            if (r.msg.msg_type() == "8" && r.msg.get(tag::ExecType).value_or("") == "F") ++trades;
            buf.erase(0, r.consumed);
            progress = true;
        }
    }
    EXPECT_GE(trades, 1);

    ::close(fd);
    srv.stop();
}
