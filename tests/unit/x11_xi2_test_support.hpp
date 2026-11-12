// SPDX-License-Identifier: MIT
//
// Shared helpers for the pure XInput2 / XIM translation tests. The suites that need the composition
// state machine live in x11_input_translation_xi2_ime_tests.cpp; both translation units link into
// the single x11_xi2_translation test executable.

#pragma once

#include "tessera/ui/input.hpp"

#include <iostream>
#include <string>

namespace x11_test {

inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "x11_xi2_translation: " << message << "\n";
    }
    return condition;
}

inline std::string payload(const tessera::ui::InputEvent& event) {
    return std::string(event.text.data(), event.text_size);
}

// Field-by-field comparison; used by the replay test so a silent field difference fails.
inline bool same_event(const tessera::ui::InputEvent& lhs, const tessera::ui::InputEvent& rhs,
                       const char* what) {
    const tessera::ui::InputEvent& left = lhs;
    const tessera::ui::InputEvent& right = rhs;
    if (left.type != right.type || left.timestamp_s != right.timestamp_s ||
        left.device_id != right.device_id || left.pointer_id != right.pointer_id ||
        left.pointer_kind != right.pointer_kind || left.x != right.x || left.y != right.y ||
        left.dx != right.dx || left.dy != right.dy || left.pressure != right.pressure ||
        left.button != right.button || left.key != right.key || left.modifiers != right.modifiers ||
        left.text_codepoint != right.text_codepoint || left.repeat != right.repeat ||
        left.is_primary != right.is_primary || left.text_size != right.text_size ||
        left.text_truncated != right.text_truncated ||
        left.selection_start != right.selection_start ||
        left.selection_end != right.selection_end || left.text != right.text) {
        std::cerr << "x11_xi2_translation: replay differs at " << what << "\n";
        return false;
    }
    return true;
}

} // namespace x11_test

// Suites defined in x11_input_translation_xi2_ime_tests.cpp.
bool ime_composition_suite();
bool ime_clamping_suite();
