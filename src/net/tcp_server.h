// SPDX-License-Identifier: MIT
#pragma once

#include "../core/order_book.h"
#include "../core/matcher.h"
#include "../core/types.h"
#include "../fix/session.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace obfix::net {

struct ServerConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{0};  // 0 = ephemeral; the server fills in the bound port
    std::string sender_comp_id{"OBFIX"};
    int heartbeat_secs{30};
    MatchAlgo algo{MatchAlgo::ProRata};
};

class TcpServer {
public:
    explicit TcpServer(ServerConfig cfg);
    ~TcpServer();

    // Bind and start accepting. Returns true on success. The actual bound
    // port (when cfg.port == 0) is available via bound_port().
    bool start();
    void stop();

    std::uint16_t bound_port() const noexcept { return bound_port_.load(); }

    // Total messages processed across all sessions (for tests/bench).
    std::uint64_t messages_in() const noexcept { return msgs_in_.load(); }
    std::uint64_t messages_out() const noexcept { return msgs_out_.load(); }

private:
    void accept_loop();
    void session_loop(int fd);

    ServerConfig cfg_;
    std::atomic<int> listen_fd_{-1};
    std::atomic<std::uint16_t> bound_port_{0};
    std::atomic<bool> running_{false};
    std::thread acceptor_;
    std::mutex sessions_mu_;
    std::vector<std::thread> session_threads_;

    std::atomic<std::uint64_t> msgs_in_{0};
    std::atomic<std::uint64_t> msgs_out_{0};

    // One shared book + matcher. Sessions feed it under a coarse lock.
    // This is intentional; ARM64 acq/rel was tried and abandoned because
    // the FIX serializer is the actual bottleneck (see bench/results).
    std::mutex book_mu_;
    OrderBook book_;
    Matcher matcher_;
    std::atomic<OrderId> next_order_id_{1};
};

}  // namespace obfix::net
