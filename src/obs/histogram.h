// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace obfix::obs {

// Logarithmic histogram for nanosecond latencies up to ~10^11 ns (~100 s).
// The bucket index is floor(log2(ns)). 64 buckets cover the full u64 range
// safely. This is not an HDR histogram; it gives quantiles accurate to
// ~factor-2, which is enough for ranking P50 vs P99.
class LogHistogram {
public:
    LogHistogram() : buckets_(64, 0) {}

    void record(std::uint64_t ns) {
        if (ns == 0) ns = 1;
        int b = bucket_for(ns);
        ++buckets_[b];
        ++count_;
        sum_ += ns;
    }

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t sum() const noexcept { return sum_; }

    // Approximate quantile. Returns the upper bound of the bucket that
    // contains the quantile.
    std::uint64_t quantile(double q) const {
        if (count_ == 0) return 0;
        std::uint64_t target = static_cast<std::uint64_t>(q * count_);
        std::uint64_t acc = 0;
        for (int b = 0; b < 64; ++b) {
            acc += buckets_[b];
            if (acc >= target) return 1ull << (b + 1);  // upper bound of bucket
        }
        return 1ull << 63;
    }

    void merge(const LogHistogram& other) {
        for (int i = 0; i < 64; ++i) buckets_[i] += other.buckets_[i];
        count_ += other.count_;
        sum_ += other.sum_;
    }

private:
    static int bucket_for(std::uint64_t ns) {
        int b = 0;
        while (ns >>= 1) ++b;
        if (b >= 64) b = 63;
        return b;
    }

    std::vector<std::uint64_t> buckets_;
    std::uint64_t count_{0};
    std::uint64_t sum_{0};
};

}  // namespace obfix::obs
