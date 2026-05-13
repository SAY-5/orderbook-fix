// SPDX-License-Identifier: MIT
#pragma once

#include "messages.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace obfix::fix {

// FIX uses ASCII SOH (0x01) as the tag-value separator on the wire. Tests
// and the docs use '|' for readability; the parser accepts either by
// configuration so the test corpus and the wire share one code path.
constexpr char kSohWire = '\x01';
constexpr char kSohText = '|';

enum class ParseError {
    None,
    Incomplete,      // not enough bytes for a full message
    BadHeader,       // missing 8=FIX.4.4 prefix or BodyLength
    BadBodyLength,   // 9= value is non-numeric or implausible
    BadCheckSum,     // 10= mismatch
    MalformedField,  // tag without value, non-numeric tag, etc.
    DuplicateField,  // same tag appears twice in one message
};

struct ParseResult {
    ParseError err{ParseError::None};
    std::size_t consumed{0};  // bytes consumed from the input buffer
    Message msg{};
};

// Compute the FIX checksum: sum of bytes (mod 256) over the message up to
// and including the SOH before "10=". Returns the integer 0-255.
unsigned checksum(std::string_view bytes);

// Format checksum as 3-digit zero-padded decimal.
std::string format_checksum(unsigned v);

class Parser {
public:
    // Construct with the SOH character expected on the wire. Use kSohWire
    // for production and kSohText for tests.
    explicit Parser(char soh = kSohWire) : soh_(soh) {}

    // Try to extract one message from `buf`. On success, `consumed` is the
    // number of bytes used; the caller advances its buffer. On
    // ParseError::Incomplete, consumed is 0.
    ParseResult parse_one(std::string_view buf) const;

    char soh() const noexcept { return soh_; }

private:
    char soh_;
};

}  // namespace obfix::fix
