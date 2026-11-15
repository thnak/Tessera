// SPDX-License-Identifier: MIT

#include "scene_state.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

// The `ui::StateScalar` alternative a declared IR type maps to, matching the generated C++ field
// `emit_snapshot.cpp` reads. `ScalarKind::List` covers `list<T>` over a scalar element.
enum class ScalarKind { Bool, Int, Float, String, List };

std::optional<ScalarKind> element_kind(std::string_view ir_type) {
    if (ir_type == "bool") {
        return ScalarKind::Bool;
    }
    if (ir_type == "int") {
        return ScalarKind::Int;
    }
    if (ir_type == "float" || ir_type == "length") {
        return ScalarKind::Float;
    }
    if (ir_type == "string" || ir_type == "color") {
        return ScalarKind::String;
    }
    return std::nullopt;
}

bool is_list_type(std::string_view ir_type) {
    constexpr std::string_view kList = "list<";
    return ir_type.size() > kList.size() + 1 && ir_type.rfind(kList, 0) == 0 &&
           ir_type.back() == '>';
}

std::string_view list_element(std::string_view ir_type) {
    constexpr std::string_view kList = "list<";
    return ir_type.substr(kList.size(), ir_type.size() - kList.size() - 1);
}

// The kind of a declared type, `list<...>` included. Nullopt means "not representable as a
// StateCell", which is the same set `snapshot_type()` in emit_snapshot.cpp rejects.
std::optional<ScalarKind> declared_kind(std::string_view ir_type) {
    if (is_list_type(ir_type)) {
        return element_kind(list_element(ir_type)).has_value()
                   ? std::optional<ScalarKind>(ScalarKind::List)
                   : std::nullopt;
    }
    return element_kind(ir_type);
}

bool scalar_matches(ScalarKind kind, const ui::StateScalar& scalar) {
    switch (kind) {
    case ScalarKind::Bool:
        return std::holds_alternative<bool>(scalar);
    case ScalarKind::Int:
        return std::holds_alternative<int>(scalar);
    case ScalarKind::Float:
        return std::holds_alternative<double>(scalar);
    case ScalarKind::String:
        return std::holds_alternative<std::string>(scalar);
    case ScalarKind::List:
        return false;
    }
    return false;
}

// One evaluator value as a state scalar. A null, a nested list or a record has no scalar form and
// is reported as "not representable" rather than flattened into something the native build would
// never hold.
std::optional<ui::StateScalar> to_scalar(const eval::Value& value) {
    if (const bool* flag = value.as_bool()) {
        return ui::StateScalar{*flag};
    }
    if (const std::int32_t* whole = value.as_int()) {
        return ui::StateScalar{static_cast<int>(*whole)};
    }
    if (const double* number = value.as_float()) {
        return ui::StateScalar{*number};
    }
    if (const std::string* text = value.as_string()) {
        return ui::StateScalar{*text};
    }
    return std::nullopt;
}

eval::Value to_value(const ui::StateScalar& scalar) {
    if (const bool* flag = std::get_if<bool>(&scalar)) {
        return eval::Value::of_bool(*flag);
    }
    if (const int* whole = std::get_if<int>(&scalar)) {
        return eval::Value::of_int(static_cast<std::int32_t>(*whole));
    }
    if (const double* number = std::get_if<double>(&scalar)) {
        return eval::Value::of_float(*number);
    }
    return eval::Value::of_string(std::get<std::string>(scalar));
}

// One declared cell of a component, over both declared vectors, in declaration order.
struct DeclaredCell {
    const std::string* id = nullptr;
    const std::string* type = nullptr;
};

std::vector<DeclaredCell> declared_cells(const ir::Component& component) {
    std::vector<DeclaredCell> cells;
    cells.reserve(component.state.size() + component.animated_values.size());
    for (const ir::State& state : component.state) {
        cells.push_back({&state.id, &state.type});
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        cells.push_back({&value.id, &value.type});
    }
    return cells;
}

bool value_fits(ScalarKind kind, const ui::StateCell& cell) {
    if (kind == ScalarKind::List) {
        return cell.is_list;
    }
    return !cell.is_list && scalar_matches(kind, cell.value);
}

} // namespace

bool carryable_type(std::string_view ir_type) noexcept {
    return declared_kind(ir_type).has_value();
}

CarriedState capture_state(const ir::Component& component, const eval::Scope& scope) {
    CarriedState carried;
    for (const DeclaredCell& declared : declared_cells(component)) {
        if (!declared_kind(*declared.type).has_value()) {
            continue; // a record cell has no StateCell form, exactly as snapshot_state() skips it
        }
        const auto found = scope.find(*declared.id);
        if (found == scope.end()) {
            continue;
        }
        const eval::Value& value = found->second;
        ui::StateCell cell;
        cell.name = *declared.id;
        if (const auto* items = value.as_list()) {
            cell.is_list = true;
            cell.items.reserve(items->size());
            bool representable = true;
            for (const eval::Value& item : *items) {
                std::optional<ui::StateScalar> scalar = to_scalar(item);
                if (!scalar.has_value()) {
                    representable = false;
                    break;
                }
                cell.items.push_back(std::move(*scalar));
            }
            if (!representable) {
                continue;
            }
        } else {
            std::optional<ui::StateScalar> scalar = to_scalar(value);
            if (!scalar.has_value()) {
                continue;
            }
            cell.value = std::move(*scalar);
        }
        carried.cells.push_back(
            {StateCellKey{component.component_id, *declared.id, *declared.type}, std::move(cell)});
    }
    return carried;
}

StateCarryReport apply_state(const ir::Component& component, const CarriedState& carried,
                             const std::vector<std::string>& live_component_ids,
                             eval::Scope& scope) {
    StateCarryReport report;
    std::vector<bool> matched(carried.cells.size(), false);
    const auto live = [&](const std::string& id) {
        return std::find(live_component_ids.begin(), live_component_ids.end(), id) !=
               live_component_ids.end();
    };

    for (const DeclaredCell& declared : declared_cells(component)) {
        const std::optional<ScalarKind> kind = declared_kind(*declared.type);
        const std::optional<ScalarKind> element = is_list_type(*declared.type)
                                                      ? element_kind(list_element(*declared.type))
                                                      : std::nullopt;
        bool satisfied = false;
        for (std::size_t index = 0; index < carried.cells.size(); ++index) {
            const CarriedCell& cell = carried.cells[index];
            if (cell.key.component_id != component.component_id ||
                cell.key.cell_id != *declared.id) {
                continue;
            }
            matched[index] = true;
            satisfied = true;
            // A carried value never overwrites a cell the caller already seeded.
            if (scope.find(*declared.id) != scope.end()) {
                break;
            }
            // The declared IR type is part of the key: a carried int in a now-float cell is a
            // conversion the native build never performs, so it is dropped and re-initialized.
            if (cell.key.ir_type != *declared.type || !kind.has_value() ||
                !value_fits(*kind, cell.value)) {
                ++report.dropped_type;
                break;
            }
            // A list cell's elements must match the declared element type.
            if (*kind == ScalarKind::List &&
                !std::all_of(
                    cell.value.items.begin(), cell.value.items.end(),
                    [&](const ui::StateScalar& item) { return scalar_matches(*element, item); })) {
                ++report.dropped_type;
                break;
            }
            if (cell.value.is_list) {
                eval::List items;
                items.reserve(cell.value.items.size());
                for (const ui::StateScalar& item : cell.value.items) {
                    items.push_back(to_value(item));
                }
                scope[*declared.id] = eval::Value::of_list(std::move(items));
            } else {
                scope[*declared.id] = to_value(cell.value.value);
            }
            ++report.carried;
            break;
        }
        if (!satisfied) {
            ++report.added; // no carried value for a declared cell: it starts at its initializer
        }
    }

    for (std::size_t index = 0; index < carried.cells.size(); ++index) {
        if (matched[index]) {
            continue;
        }
        // The component id is the identity. A cell whose component is gone is dropped, never
        // offered to a component that took its place; a cell of a live component that no longer
        // declares that id is a removed cell. The two are counted separately because they answer
        // different questions ("did the component survive?" and "did the cell survive?").
        if (live(carried.cells[index].key.component_id)) {
            ++report.dropped_cell;
        } else {
            ++report.dropped_component;
        }
    }
    return report;
}

} // namespace tessera::compiler::codegen::cli
