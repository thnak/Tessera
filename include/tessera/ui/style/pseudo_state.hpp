// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::ui::style {

// Pseudo-state bits used by selector matching. The input layer owns the interaction states
// (`Hover`, `Active`, `Focus`, `FocusVisible`); component state owns the rest. Keeping the bits
// in one small header lets the scene graph expose state without depending on the stylesheet
// engine's parsing and cascade code.
enum class PseudoState : std::uint32_t {
    None = 0,
    Hover = 1u << 0,
    Active = 1u << 1,
    Focus = 1u << 2,
    FocusVisible = 1u << 3,
    Disabled = 1u << 4,
    Checked = 1u << 5,
    Required = 1u << 6,
};

constexpr PseudoState operator|(PseudoState lhs, PseudoState rhs) {
    return static_cast<PseudoState>(static_cast<std::uint32_t>(lhs) |
                                    static_cast<std::uint32_t>(rhs));
}

constexpr PseudoState operator&(PseudoState lhs, PseudoState rhs) {
    return static_cast<PseudoState>(static_cast<std::uint32_t>(lhs) &
                                    static_cast<std::uint32_t>(rhs));
}

constexpr PseudoState operator~(PseudoState value) {
    return static_cast<PseudoState>(~static_cast<std::uint32_t>(value));
}

constexpr PseudoState& operator|=(PseudoState& lhs, PseudoState rhs) {
    lhs = lhs | rhs;
    return lhs;
}

constexpr PseudoState& operator&=(PseudoState& lhs, PseudoState rhs) {
    lhs = lhs & rhs;
    return lhs;
}

constexpr bool has_state(PseudoState value, PseudoState flag) {
    return (value & flag) != PseudoState::None;
}

} // namespace tessera::ui::style
