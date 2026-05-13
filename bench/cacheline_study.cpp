// SPDX-License-Identifier: MIT
//
// Cache-line padding microbench. Two variants of the hot match path are
// compiled side-by-side: one with `alignas(64)` on Order and PriceLevel,
// one without. The bench replays a fixed sequence of submits against each
// variant and reports P99 latency for both.
//
// Output: JSON with two records (`baseline_p99_ns`, `aligned_p99_ns`,
// `delta_pct`) on stdout, or to a path given by --out.
//
// Hardware sensitivity: the result on a single-socket M2 Pro hides any
// false-sharing effect because the matcher is serialized. The number is
// committed to bench/results/cacheline_study.json so the comparison is
// visible without re-running.

#include "fix/messages.h"
#include "fix/parser.h"
#include "fix/serializer.h"
#include "obs/histogram.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace bench {

// ---- Variant A: baseline (struct layout matches production) ----
namespace baseline {

struct Order {
    std::uint64_t id{0};
    char side{'1'};
    std::int64_t price{0};
    std::uint64_t orig_qty{0};
    std::uint64_t leaves_qty{0};
    Order* prev{nullptr};
    Order* next{nullptr};
};

struct PriceLevel {
    Order* head{nullptr};
    Order* tail{nullptr};
    std::size_t count{0};
    std::uint64_t total_qty{0};
};

inline void push(PriceLevel& l, Order* o) {
    o->prev = l.tail;
    o->next = nullptr;
    if (l.tail)
        l.tail->next = o;
    else
        l.head = o;
    l.tail = o;
    ++l.count;
    l.total_qty += o->leaves_qty;
}

inline std::uint64_t fifo_fill(PriceLevel& l, std::uint64_t want) {
    std::uint64_t filled = 0;
    for (Order* o = l.head; o && filled < want; o = o->next) {
        std::uint64_t q = std::min(want - filled, o->leaves_qty);
        o->leaves_qty -= q;
        l.total_qty -= q;
        filled += q;
    }
    return filled;
}

}  // namespace baseline

// ---- Variant B: alignas(64) on Order and PriceLevel ----
namespace aligned {

struct alignas(64) Order {
    std::uint64_t id{0};
    char side{'1'};
    std::int64_t price{0};
    std::uint64_t orig_qty{0};
    std::uint64_t leaves_qty{0};
    Order* prev{nullptr};
    Order* next{nullptr};
};

struct alignas(64) PriceLevel {
    Order* head{nullptr};
    Order* tail{nullptr};
    std::size_t count{0};
    std::uint64_t total_qty{0};
};

inline void push(PriceLevel& l, Order* o) {
    o->prev = l.tail;
    o->next = nullptr;
    if (l.tail)
        l.tail->next = o;
    else
        l.head = o;
    l.tail = o;
    ++l.count;
    l.total_qty += o->leaves_qty;
}

inline std::uint64_t fifo_fill(PriceLevel& l, std::uint64_t want) {
    std::uint64_t filled = 0;
    for (Order* o = l.head; o && filled < want; o = o->next) {
        std::uint64_t q = std::min(want - filled, o->leaves_qty);
        o->leaves_qty -= q;
        l.total_qty -= q;
        filled += q;
    }
    return filled;
}

}  // namespace aligned

}  // namespace bench

namespace {

struct Result {
    obfix::obs::LogHistogram hist;
    double total_seconds{0.0};
    std::size_t n_iters{0};
};

template <typename Order, typename PriceLevel, void (*PUSH)(PriceLevel&, Order*),
          std::uint64_t (*FILL)(PriceLevel&, std::uint64_t)>
Result run_variant(std::size_t n_iters, std::size_t resting_per_level) {
    Result r;
    r.n_iters = n_iters;
    // Allocate a contiguous pool of orders so the access pattern stays
    // realistic. Random qtys per order; aggressor walks all of them.
    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<std::uint64_t> qd(1, 1000);
    std::vector<std::unique_ptr<Order>> pool;
    pool.reserve(n_iters * resting_per_level);

    auto wall0 = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < n_iters; ++i) {
        PriceLevel level;
        for (std::size_t k = 0; k < resting_per_level; ++k) {
            auto o = std::make_unique<Order>();
            o->id = i * resting_per_level + k;
            o->orig_qty = qd(rng);
            o->leaves_qty = o->orig_qty;
            PUSH(level, o.get());
            pool.push_back(std::move(o));
        }
        std::uint64_t want = level.total_qty;
        auto t0 = std::chrono::steady_clock::now();
        std::uint64_t filled = FILL(level, want);
        auto t1 = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        r.hist.record(static_cast<std::uint64_t>(ns));
        // Defeat optimizer.
        asm volatile("" : : "r"(filled) : "memory");
    }
    auto wall1 = std::chrono::steady_clock::now();
    r.total_seconds =
        static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(wall1 - wall0).count()) /
        1e9;
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n_iters = 50000;
    std::size_t resting = 8;
    std::string out_path;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--n" && i + 1 < argc)
            n_iters = static_cast<std::size_t>(std::stoull(argv[++i]));
        else if (a == "--resting" && i + 1 < argc)
            resting = static_cast<std::size_t>(std::stoull(argv[++i]));
        else if (a == "--out" && i + 1 < argc)
            out_path = argv[++i];
        else if (a == "--smoke") {
            n_iters = 5000;
        }
    }

    // Warm caches with a quick throwaway pass.
    auto warm = run_variant<bench::baseline::Order, bench::baseline::PriceLevel,
                            bench::baseline::push, bench::baseline::fifo_fill>(1000, resting);
    (void)warm;

    auto r_base = run_variant<bench::baseline::Order, bench::baseline::PriceLevel,
                              bench::baseline::push, bench::baseline::fifo_fill>(n_iters, resting);
    auto r_alig = run_variant<bench::aligned::Order, bench::aligned::PriceLevel,
                              bench::aligned::push, bench::aligned::fifo_fill>(n_iters, resting);

    auto b50 = r_base.hist.quantile(0.50);
    auto b99 = r_base.hist.quantile(0.99);
    auto a50 = r_alig.hist.quantile(0.50);
    auto a99 = r_alig.hist.quantile(0.99);
    // Wall-time mean is the most stable signal at sub-bucket resolution.
    double base_mean_ns =
        r_base.n_iters ? r_base.total_seconds * 1e9 / static_cast<double>(r_base.n_iters) : 0.0;
    double alig_mean_ns =
        r_alig.n_iters ? r_alig.total_seconds * 1e9 / static_cast<double>(r_alig.n_iters) : 0.0;
    double delta_mean =
        base_mean_ns == 0.0 ? 0.0 : (alig_mean_ns - base_mean_ns) / base_mean_ns * 100.0;
    double delta_p99 = b99 == 0 ? 0.0
                                : (static_cast<double>(a99) - static_cast<double>(b99)) /
                                      static_cast<double>(b99) * 100.0;

    char buf[1024];
    std::snprintf(buf, sizeof(buf),
                  "{\n"
                  "  \"n_iters\": %zu,\n"
                  "  \"resting_per_level\": %zu,\n"
                  "  \"baseline_mean_ns\": %.2f,\n"
                  "  \"baseline_p50_ns\": %llu,\n"
                  "  \"baseline_p99_ns\": %llu,\n"
                  "  \"aligned_mean_ns\": %.2f,\n"
                  "  \"aligned_p50_ns\": %llu,\n"
                  "  \"aligned_p99_ns\": %llu,\n"
                  "  \"delta_mean_pct\": %.2f,\n"
                  "  \"delta_p99_pct\": %.2f\n"
                  "}\n",
                  n_iters, resting, base_mean_ns, static_cast<unsigned long long>(b50),
                  static_cast<unsigned long long>(b99), alig_mean_ns,
                  static_cast<unsigned long long>(a50), static_cast<unsigned long long>(a99),
                  delta_mean, delta_p99);
    if (out_path.empty()) {
        std::fputs(buf, stdout);
    } else {
        std::ofstream f(out_path);
        f << buf;
        std::fputs(buf, stdout);
    }
    return 0;
}
