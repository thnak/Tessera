// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::ui {

// One scalar a component's state cell can hold: a `bool`, an `int`, a `float` (C++ `double`) or a
// string (`string`, `color` and `length` cells are stored as the IR stores them).
using StateScalar = std::variant<bool, int, double, std::string>;

// One state cell of a generated component, by the IR id it has in the component (`state::count`,
// `anim::fade`). A list cell carries its elements in `items` and leaves `value` at its default.
struct StateCell {
    std::string name;
    bool is_list = false;
    StateScalar value;
    std::vector<StateScalar> items;
};

// Every `useState` and `useAnimatedValue` cell, in declaration order. Generated components return
// it from `snapshot_state()`, so a test or a parity check can read the whole state without
// reflection (docs/design/compiler.md section 11). It is a copy: build it at checkpoints, never on
// the frame path.
using StateSnapshot = std::vector<StateCell>;

[[nodiscard]] inline StateCell state_cell(std::string name, bool value) {
    return StateCell{std::move(name), false, StateScalar{value}, {}};
}
[[nodiscard]] inline StateCell state_cell(std::string name, int value) {
    return StateCell{std::move(name), false, StateScalar{value}, {}};
}
[[nodiscard]] inline StateCell state_cell(std::string name, double value) {
    return StateCell{std::move(name), false, StateScalar{value}, {}};
}
[[nodiscard]] inline StateCell state_cell(std::string name, const std::string& value) {
    return StateCell{std::move(name), false, StateScalar{value}, {}};
}
template<typename T>
[[nodiscard]] StateCell state_cell(std::string name, const std::vector<T>& values) {
    StateCell cell{std::move(name), true, StateScalar{}, {}};
    cell.items.reserve(values.size());
    for (const T& element : values) {
        cell.items.emplace_back(element);
    }
    return cell;
}

} // namespace tessera::ui
