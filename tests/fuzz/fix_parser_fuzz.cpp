// SPDX-License-Identifier: MIT
// libFuzzer harness for the FIX parser. Crash-free is the only invariant.
#include "fix/parser.h"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    obfix::fix::Parser p(obfix::fix::kSohWire);
    std::string_view sv(reinterpret_cast<const char*>(data), size);
    while (!sv.empty()) {
        auto r = p.parse_one(sv);
        if (r.consumed == 0) break;
        sv.remove_prefix(r.consumed);
    }
    return 0;
}
