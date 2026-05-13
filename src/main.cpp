// SPDX-License-Identifier: MIT
#include "net/tcp_server.h"
#include "obs/logger.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {
std::atomic<bool> g_stop{false};
void on_sig(int) {
    g_stop.store(true);
}
}  // namespace

int main(int argc, char** argv) {
    obfix::net::ServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 9876;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc)
            cfg.port = static_cast<std::uint16_t>(std::stoi(argv[++i]));
        else if (a == "--host" && i + 1 < argc)
            cfg.host = argv[++i];
        else if (a == "--algo" && i + 1 < argc) {
            std::string v = argv[++i];
            cfg.algo = (v == "fifo") ? obfix::MatchAlgo::Fifo : obfix::MatchAlgo::ProRata;
        }
    }
    if (const char* env = std::getenv("MATCH_ALGO")) {
        cfg.algo =
            (std::strcmp(env, "fifo") == 0) ? obfix::MatchAlgo::Fifo : obfix::MatchAlgo::ProRata;
    }

    obfix::net::TcpServer srv(cfg);
    if (!srv.start()) {
        obfix::obs::log_warn("main", "server failed to start");
        return 1;
    }
    obfix::obs::log_info(
        "main", std::string("listening on ") + cfg.host + ":" + std::to_string(srv.bound_port()));
    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);
    while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    srv.stop();
    return 0;
}
