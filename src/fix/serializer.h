// SPDX-License-Identifier: MIT
#pragma once

#include "messages.h"
#include "parser.h"

#include <string>
#include <utility>
#include <vector>

namespace obfix::fix {

// Build a FIX message from an ordered list of (tag, value) pairs. The
// serializer prepends 8=FIX.4.4 and 9=<bodylen> automatically and appends
// 10=NNN at the end. Callers should pass MsgType (35) as the first body
// field per the spec.
class Serializer {
public:
    explicit Serializer(char soh = kSohWire) : soh_(soh) {}

    // body must NOT include 8, 9 or 10. Returns the full wire bytes.
    std::string build(const std::vector<std::pair<int, std::string>>& body) const;

    char soh() const noexcept { return soh_; }

private:
    char soh_;
};

}  // namespace obfix::fix
