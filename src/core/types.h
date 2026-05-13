// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>

namespace obfix {

using OrderId = std::uint64_t;
using Quantity = std::uint64_t;
using Price = std::int64_t;  // integer ticks, e.g. cents
using SeqNum = std::uint64_t;
using SessionId = std::uint64_t;
using Timestamp = std::uint64_t;  // ns since epoch

enum class Side : char {
    Buy = '1',
    Sell = '2',
};

enum class OrdType : char {
    Limit = '2',
    // Market is intentionally not supported (no last trade price tracker)
};

enum class MatchAlgo {
    ProRata,
    Fifo,
};

struct ClOrdID {
    std::string value;
    bool operator==(const ClOrdID& o) const { return value == o.value; }
};

}  // namespace obfix
