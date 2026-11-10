// SPDX-License-Identifier: MIT
//
// Shared assertions and byte helpers for the AT-SPI2 D-Bus wire-client tests. Every executable in
// this directory links only `tessera_accessibility` and runs without a bus daemon.

#pragma once

#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace a11y_dbus_test {

inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "a11y dbus: " << message << "\n";
    }
    return condition;
}

inline bool expect_equal(std::string_view actual, std::string_view expected, const char* message) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "a11y dbus: " << message << " (actual=\"" << actual << "\", expected=\""
              << expected << "\")\n";
    return false;
}

// Uppercase hex, one pair per byte: the golden form used by the wire-format tests.
inline std::string hex(std::span<const std::uint8_t> bytes) {
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t byte : bytes) {
        out.push_back(kDigits[(byte >> 4) & 0x0FU]);
        out.push_back(kDigits[byte & 0x0FU]);
    }
    return out;
}

inline std::string hex(std::string_view bytes) {
    return hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),
                                             bytes.size()));
}

inline std::vector<std::uint8_t> from_hex(std::string_view text) {
    auto value_of = [](char digit) -> int {
        if (digit >= '0' && digit <= '9') {
            return digit - '0';
        }
        if (digit >= 'a' && digit <= 'f') {
            return digit - 'a' + 10;
        }
        if (digit >= 'A' && digit <= 'F') {
            return digit - 'A' + 10;
        }
        return -1;
    };
    std::vector<std::uint8_t> out;
    out.reserve(text.size() / 2);
    for (std::size_t index = 0; index + 1 < text.size(); index += 2) {
        const int high = value_of(text[index]);
        const int low = value_of(text[index + 1]);
        if (high < 0 || low < 0) {
            std::cerr << "a11y dbus: bad hex fixture \"" << text << "\"\n";
            return {};
        }
        out.push_back(static_cast<std::uint8_t>((high << 4) | low));
    }
    return out;
}

// ASCII text as bytes, for feeding fixtures/transports.
inline std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

inline std::string text_of(std::span<const std::uint8_t> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

} // namespace a11y_dbus_test
