// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::ui {

// Bitmask of dirty domains. The zero enumerator is named Clean instead of None because Xlib
// defines a global `None` macro (0L); translation units that include X11 (for example through the
// Vulkan Xlib surface) would rewrite a `None` enumerator before the compiler sees it.
enum class Dirty : std::uint8_t {
    Clean = 0,
    Style = 1u << 0,
    Layout = 1u << 1,
    Paint = 1u << 2,
    Render = 1u << 3,
    Resource = 1u << 4,
    Gpu = 1u << 5,
    All = (1u << 6) - 1u,
};

static_assert(static_cast<std::uint8_t>(Dirty::Clean) == 0,
              "Dirty::Clean must stay the zero value");

constexpr Dirty operator|(Dirty lhs, Dirty rhs) {
    return static_cast<Dirty>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

constexpr Dirty operator&(Dirty lhs, Dirty rhs) {
    return static_cast<Dirty>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

constexpr Dirty operator~(Dirty value) {
    return static_cast<Dirty>(~static_cast<std::uint8_t>(value) &
                              static_cast<std::uint8_t>(Dirty::All));
}

constexpr Dirty& operator|=(Dirty& lhs, Dirty rhs) {
    lhs = lhs | rhs;
    return lhs;
}

constexpr bool has_flag(Dirty value, Dirty flag) {
    return (value & flag) != Dirty::Clean;
}

constexpr bool has_any(Dirty value, Dirty flags) {
    return (value & flags) != Dirty::Clean;
}

} // namespace tessera::ui
