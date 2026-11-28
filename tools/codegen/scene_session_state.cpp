// SPDX-License-Identifier: MIT
//
// The preview session's state cells as values: `state()` (what generated `snapshot_state()`
// returns) and `capture()` (what the next evaluation inherits). Split out of `scene_session.cpp`,
// which was over the source-size limit; behavior unchanged.

#include "scene_session.hpp"
#include "scene_state.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;

} // namespace

ui::StateSnapshot PreviewSession::state() const {
    // The same cells `snapshot_state()` emits: declaration order, one `ui::StateCell` per cell.
    // Produced from the evaluator's scope rather than a generated field, but it is the identical
    // representation, so `parity::run_parity` can compare it directly.
    ui::StateSnapshot snapshot;
    for (const ir::State& cell : component_.state) {
        append_cell(snapshot, cell.id, cell.type);
    }
    for (const ir::AnimatedValue& cell : component_.animated_values) {
        append_cell(snapshot, cell.id, cell.type);
    }
    return snapshot;
}

void PreviewSession::append_cell(ui::StateSnapshot& out, const std::string& id,
                                 const std::string& type) const {
    if (!carryable_type(type)) {
        return; // a record cell has no StateCell form, matching emit_snapshot.cpp
    }
    const auto found = scope_.find(id);
    if (found == scope_.end()) {
        return;
    }
    const eval::Value& value = found->second;
    if (const eval::List* items = value.as_list()) {
        // A list cell keeps its elements; the declared element type selects the C++ vector the
        // generated snapshot would build.
        if (type == "list<bool>") {
            std::vector<bool> out_items;
            out_items.reserve(items->size());
            for (const eval::Value& item : *items) {
                if (const bool* flag = item.as_bool()) {
                    out_items.push_back(*flag);
                }
            }
            out.push_back(ui::state_cell(id, out_items));
        } else if (type == "list<int>") {
            std::vector<int> out_items;
            out_items.reserve(items->size());
            for (const eval::Value& item : *items) {
                if (const std::int32_t* whole = item.as_int()) {
                    out_items.push_back(static_cast<int>(*whole));
                }
            }
            out.push_back(ui::state_cell(id, out_items));
        } else {
            std::vector<double> out_items;
            out_items.reserve(items->size());
            for (const eval::Value& item : *items) {
                out_items.push_back(item.number());
            }
            out.push_back(ui::state_cell(id, out_items));
        }
        return;
    }
    if (const bool* flag = value.as_bool()) {
        out.push_back(ui::state_cell(id, *flag));
    } else if (const std::int32_t* whole = value.as_int()) {
        out.push_back(ui::state_cell(id, static_cast<int>(*whole)));
    } else if (const double* number = value.as_float()) {
        out.push_back(ui::state_cell(id, *number));
    } else if (const std::string* text = value.as_string()) {
        out.push_back(ui::state_cell(id, *text));
    }
}

CarriedState PreviewSession::capture() const {
    return capture_state(component_, scope_);
}

} // namespace tessera::compiler::codegen::cli
