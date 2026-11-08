// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::ui {

enum class Dirty : std::uint8_t {
    None = 0,
    Style = 1u << 0,
    Layout = 1u << 1,
    Paint = 1u << 2,
    Render = 1u << 3,
    Resource = 1u << 4,
    Gpu = 1u << 5,
    All = (1u << 6) - 1u,
};

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
    return (value & flag) != Dirty::None;
}

} // namespace tessera::ui
