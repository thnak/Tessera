// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/ir/component.hpp"
#include "tessera/ui/scene/state_snapshot.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Component state across source edits (issue #168, P4 of docs/design/reference-equantic-ui.md
// section 7.4; docs/specs/designer-studio-v1.md section 4 and docs/design/designer.md section 4,
// "preserve state by component identity + key").
//
// The rule this file implements, stated once so the test can quote it:
//
//   A state cell is carried across an edit when, and only when, the new evaluation has a component
//   with the SAME component id (automation-ids-v1 section 2.1: the salted hash of source path plus
//   component name) that declares a cell with the SAME cell id (`state::count`, `anim::fade`: the
//   IR id lowering derives from the declared name) and the SAME IR type. Everything else starts at
//   its initializer.
//
// Why each part:
//   * The component id, not the component's index or its name alone, is the identity. Inserting,
//     deleting or reordering components therefore cannot hand one component's state to another,
//     and a component whose source path changed (moved file) is a different component, so its
//     state is not carried. That is the "state for components that disappear must not be handed to
//     a different component" requirement: a removed component's cells carry a component id the new
//     evaluation does not have, so they are dropped (`dropped_component`).
//   * The cell id, not the cell's position, is the cell identity. Reordering `useState` calls or
//     inserting a new one above keeps every existing cell's value; renaming the variable makes it a
//     different cell, which correctly starts at its initializer.
//   * The IR type is part of the key because a cell's C++ field type is generated from it: carrying
//     an `int` value into a `float` cell would be a silent conversion the native build never does.
//   * The initializer expression is deliberately NOT part of the key: editing `useState(0)` to
//     `useState(10)` re-runs the initializer only for a cell that has no carried value, so a live
//     counter survives the edit instead of jumping back to 0.
//
// Not in this key, and why: the list-item `key` of `automation-ids-v1` section 1 (`(uiId,
// itemKey)`). This slice does not evaluate a repeated child (`scene_build.cpp` reports a loop as
// TESSERA_SCENE_UNSUPPORTED, docs/design/compiler-wasm.md section 7.1), so no component instance
// with an item key can exist yet. When loops are evaluated the key extends to
// `(componentId, itemKey, cellId)`; adding it later only narrows the matches, so a carried value
// that is correct today stays correct.
namespace tessera::compiler::codegen::cli {

namespace eval = tessera::compiler::eval;
namespace ir = tessera::compiler::ir;

// The identity of one carried cell: `componentId` is a decimal string (the IR stores ids as
// strings, automation-ids-v1 section 1), `cell_id` is the IR state/anim id, and `ir_type` is the
// declared type at capture time.
struct StateCellKey {
    std::string component_id;
    std::string cell_id;
    std::string ir_type;

    friend bool operator==(const StateCellKey&, const StateCellKey&) = default;
};

struct CarriedCell {
    StateCellKey key;
    ui::StateCell value;
};

// The state captured from one evaluation, in declaration order (the order
// `snapshot_state()` emits and the parity runner compares). It is plain data: a caller may keep it
// across an evaluation and hand it to the next one, which is exactly what the preview session does
// on a source edit.
struct CarriedState {
    std::vector<CarriedCell> cells;
};

// Why a carried cell did not survive. Every count is about the *carried* side except `added`, so
// `carried + dropped_component + dropped_cell + dropped_type` equals the number of cells handed in,
// and a test can assert the split rather than a total.
struct StateCarryReport {
    std::size_t carried = 0;
    // The carried component id is not in the new evaluation (the component was removed, renamed or
    // moved): its state is dropped instead of being offered to whatever component took its place.
    std::size_t dropped_component = 0;
    // The component survives but no longer declares this cell.
    std::size_t dropped_cell = 0;
    // The component declares the id, but the type (or the carried value's runtime kind) differs.
    std::size_t dropped_type = 0;
    // Declared cells with no carried value: they start at their initializer.
    std::size_t added = 0;
};

// Captures every `useState`/`useAnimatedValue` cell of `component` that `scope` holds a value for
// and whose value is representable as a `ui::StateCell` (a scalar or a flat list of scalars, the
// same set `snapshot_state()` emits). A cell whose value is null or a nested list is skipped
// rather than approximated.
[[nodiscard]] CarriedState capture_state(const ir::Component& component, const eval::Scope& scope);

// Seeds `scope` from `carried`: for every carried cell that matches this component by identity, id
// and type, inserts the carried value under its cell id. The caller initializes the remaining cells
// from their initializers (`build_scene()` fills exactly the missing ones), so `scope` may be empty
// on entry. Never overwrites a cell already in `scope`.
//
// `live_component_ids` is every component id the new evaluation produced. It is what separates a
// carried cell whose component is gone (`dropped_component`: the state must not be handed to a
// different component) from one whose component survives but no longer declares that cell
// (`dropped_cell`). Passing the whole evaluation rather than just `component` is deliberate: with
// one id the two cases are indistinguishable, and telling them apart is the rule being tested.
[[nodiscard]] StateCarryReport apply_state(const ir::Component& component,
                                           const CarriedState& carried,
                                           const std::vector<std::string>& live_component_ids,
                                           eval::Scope& scope);

// The IR types whose generated C++ field is a `ui::StateScalar`, matching `emit_snapshot.cpp`'s
// filter: the cells a snapshot (and therefore a carry) can represent.
[[nodiscard]] bool carryable_type(std::string_view ir_type) noexcept;

} // namespace tessera::compiler::codegen::cli
