// SPDX-License-Identifier: MIT
#pragma once

#include <cstdio>
#include <string_view>

namespace obfix::obs {

// Single-threaded structured logger. The TCP server is one-thread-per-
// session so locking is not needed.
inline void log_info(std::string_view tag, std::string_view msg) {
    std::fprintf(stderr, "[INFO] %.*s: %.*s\n", static_cast<int>(tag.size()), tag.data(),
                 static_cast<int>(msg.size()), msg.data());
}

inline void log_warn(std::string_view tag, std::string_view msg) {
    std::fprintf(stderr, "[WARN] %.*s: %.*s\n", static_cast<int>(tag.size()), tag.data(),
                 static_cast<int>(msg.size()), msg.data());
}

}  // namespace obfix::obs
