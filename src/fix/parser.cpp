// SPDX-License-Identifier: MIT
#include "parser.h"

#include <charconv>
#include <cstdio>

namespace obfix::fix {

unsigned checksum(std::string_view bytes) {
    unsigned s = 0;
    for (unsigned char c : bytes) s += c;
    return s & 0xFFu;
}

std::string format_checksum(unsigned v) {
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%03u", v & 0xFFu);
    return std::string(buf, 3);
}

namespace {

// Parse an unsigned integer from [begin, end). Returns nullopt on any
// non-digit or empty input.
std::optional<std::uint64_t> parse_uint(std::string_view s) {
    if (s.empty()) return std::nullopt;
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

}  // namespace

ParseResult Parser::parse_one(std::string_view buf) const {
    ParseResult r;
    if (buf.size() < 20) {
        r.err = ParseError::Incomplete;
        return r;
    }

    // FIX message must start with "8=FIX.4.4" + SOH.
    static constexpr std::string_view kBegin = "8=FIX.4.4";
    if (buf.substr(0, kBegin.size()) != kBegin) {
        r.err = ParseError::BadHeader;
        return r;
    }
    std::size_t pos = kBegin.size();
    if (pos >= buf.size() || buf[pos] != soh_) {
        r.err = ParseError::BadHeader;
        return r;
    }
    ++pos;

    // Next field must be "9=<n>" + SOH.
    if (pos + 2 >= buf.size() || buf[pos] != '9' || buf[pos + 1] != '=') {
        r.err = ParseError::BadHeader;
        return r;
    }
    pos += 2;
    std::size_t body_len_end = buf.find(soh_, pos);
    if (body_len_end == std::string_view::npos) {
        r.err = ParseError::Incomplete;
        return r;
    }
    auto body_len_opt = parse_uint(buf.substr(pos, body_len_end - pos));
    if (!body_len_opt) {
        r.err = ParseError::BadBodyLength;
        return r;
    }
    std::uint64_t body_len = *body_len_opt;
    pos = body_len_end + 1;

    // The body has exactly `body_len` bytes. After it comes "10=NNN" + SOH.
    if (pos + body_len + 7 > buf.size()) {
        r.err = ParseError::Incomplete;
        return r;
    }
    std::size_t body_start = pos;
    std::size_t body_end = body_start + body_len;
    if (buf[body_end] != '1' || buf[body_end + 1] != '0' || buf[body_end + 2] != '=') {
        r.err = ParseError::BadCheckSum;
        return r;
    }
    auto cs_opt = parse_uint(buf.substr(body_end + 3, 3));
    if (!cs_opt) {
        r.err = ParseError::BadCheckSum;
        return r;
    }
    if (buf[body_end + 6] != soh_) {
        r.err = ParseError::BadCheckSum;
        return r;
    }
    unsigned want = static_cast<unsigned>(*cs_opt);
    unsigned got = checksum(buf.substr(0, body_end));
    if (want != got) {
        r.err = ParseError::BadCheckSum;
        return r;
    }

    // Now walk the header (8=, 9=) + body and populate the map.
    // We re-parse from offset 0 so tag 8 and 9 are visible to consumers.
    std::size_t i = 0;
    std::size_t total = body_end + 7;
    while (i < body_end) {
        // Tag.
        std::size_t eq = buf.find('=', i);
        if (eq == std::string_view::npos || eq >= body_end) {
            r.err = ParseError::MalformedField;
            return r;
        }
        auto tag_opt = parse_uint(buf.substr(i, eq - i));
        if (!tag_opt) {
            r.err = ParseError::MalformedField;
            return r;
        }
        std::size_t val_start = eq + 1;
        std::size_t soh_at = buf.find(soh_, val_start);
        if (soh_at == std::string_view::npos || soh_at > body_end) {
            r.err = ParseError::MalformedField;
            return r;
        }
        int t = static_cast<int>(*tag_opt);
        std::string val(buf.substr(val_start, soh_at - val_start));
        auto [it, inserted] = r.msg.fields.emplace(t, std::move(val));
        if (!inserted) {
            r.err = ParseError::DuplicateField;
            return r;
        }
        i = soh_at + 1;
    }
    // Stash the checksum too so round-tripping does not lose it.
    r.msg.fields.emplace(tag::CheckSum, format_checksum(want));
    r.consumed = total;
    r.err = ParseError::None;
    return r;
}

}  // namespace obfix::fix
