// SPDX-License-Identifier: MIT
#include "serializer.h"

#include <string>

namespace obfix::fix {

std::string Serializer::build(const std::vector<std::pair<int, std::string>>& body) const {
    // First serialize the body fields (35=..., 49=..., etc.) without 8/9/10.
    std::string b;
    b.reserve(64 * body.size());
    for (const auto& [tag, val] : body) {
        b += std::to_string(tag);
        b += '=';
        b += val;
        b += soh_;
    }
    // Header: 8=FIX.4.4|9=<len>|
    std::string header = "8=FIX.4.4";
    header += soh_;
    header += "9=";
    header += std::to_string(b.size());
    header += soh_;

    // Checksum is over header + body.
    std::string out;
    out.reserve(header.size() + b.size() + 7);
    out += header;
    out += b;
    unsigned cs = checksum(out);
    out += "10=";
    out += format_checksum(cs);
    out += soh_;
    return out;
}

}  // namespace obfix::fix
