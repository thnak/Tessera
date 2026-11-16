// SPDX-License-Identifier: MIT
//
// Value-type helpers for the `net` module (docs/specs/networking-v1.md section 2 and section 3
// item 7; issue #197).

#include "tessera/appfw/net_types.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace tessera::appfw {

namespace {

// ASCII-only by design: header names are tokens (RFC 9110 section 5.1), so a locale-dependent
// `std::tolower` would be both wrong and a determinism hazard.
constexpr char ascii_lower(char value) noexcept {
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

} // namespace

bool header_name_equals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(left[index]) != ascii_lower(right[index])) {
            return false;
        }
    }
    return true;
}

const std::string* find_header(const NetHeaders& headers, std::string_view name) noexcept {
    for (const auto& [key, value] : headers) {
        if (header_name_equals(key, name)) {
            return &value;
        }
    }
    return nullptr;
}

bool has_header(const NetHeaders& headers, std::string_view name) noexcept {
    return find_header(headers, name) != nullptr;
}

void set_header(NetHeaders& headers, std::string_view name, std::string value) {
    bool replaced = false;
    for (auto iterator = headers.begin(); iterator != headers.end();) {
        if (!header_name_equals(iterator->first, name)) {
            ++iterator;
            continue;
        }
        if (!replaced) {
            // The first slot keeps the caller's name spelling and send position (section 3 item 7:
            // "stored as sent"), and only the value changes.
            iterator->second = std::move(value);
            replaced = true;
            ++iterator;
            continue;
        }
        iterator = headers.erase(iterator);
    }
    if (!replaced) {
        headers.emplace_back(std::string(name), std::move(value));
    }
}

std::size_t remove_header(NetHeaders& headers, std::string_view name) {
    std::size_t removed = 0;
    for (auto iterator = headers.begin(); iterator != headers.end();) {
        if (header_name_equals(iterator->first, name)) {
            iterator = headers.erase(iterator);
            ++removed;
            continue;
        }
        ++iterator;
    }
    return removed;
}

std::string default_user_agent() {
    // "a fixed `Tessera/<version>`" (section 3 item 7). The version comes from the build, so it
    // cannot drift from the release the binary belongs to; the fallback keeps a standalone compile
    // (a test that includes the header outside CMake) well-formed.
#if defined(TESSERA_APPFW_VERSION)
    return std::string("Tessera/") + TESSERA_APPFW_VERSION;
#else
    return std::string("Tessera/0.0.0");
#endif
}

} // namespace tessera::appfw
