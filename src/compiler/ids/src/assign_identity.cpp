// SPDX-License-Identifier: MIT

#include "node_walk.hpp"
#include "tessera/compiler/ids/assign_identity.hpp"
#include "tessera/ui/automation/id.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::ids {
namespace {

namespace automation = tessera::ui::automation;

// What the list key of a node contributes to its hash: a literal key as text, nothing otherwise
// (an expression key belongs to a loop template, which is one node).
std::string key_text(const ir::Node& node) {
    if (!node.key || node.key->kind != ir::ExprKind::Literal) {
        return {};
    }
    const ir::LiteralValue& literal = node.key->literal;
    if (const auto* text = std::get_if<std::string>(&literal)) {
        return *text;
    }
    if (const auto* whole = std::get_if<std::int64_t>(&literal)) {
        return std::to_string(*whole);
    }
    return {};
}

std::string describe(const automation::IdError& error) {
    return std::string(automation::to_string(error.code)) + ": " + error.detail;
}

} // namespace

namespace {

// Shared body of `assign_identity` and `assign_library_identity`: components first, then the nodes
// that have no uiId yet, in document order. `mirror_automation_id` is off for framework library
// components, whose nodes are internal and carry no explicit automation target.
std::expected<void, IdentityError>
assign_module_identity(ir::IrModule& module, std::string_view salt, bool mirror_automation_id) {
    if (salt.empty()) {
        return std::unexpected(
            IdentityError{"the project salt (tessera.json `namespace`) is empty"});
    }

    // Every component is an input, in module order: a repeated (source path, name) pair is
    // disambiguated by its occurrence among same-named components of that file, never by the
    // absolute index in the module. Components that already have an id (library components) keep
    // it; the computed vector is still aligned with `module.components` so the occurrence counting
    // is module-wide and stable.
    std::vector<automation::ComponentIdInput> component_inputs;
    component_inputs.reserve(module.components.size());
    for (const ir::Component& component : module.components) {
        component_inputs.push_back({component.source.file, component.name});
    }
    auto component_ids = automation::assign_component_ids(salt, component_inputs);
    if (!component_ids) {
        return std::unexpected(IdentityError{describe(component_ids.error())});
    }

    // Then the nodes that have no uiId yet, in document order per component.
    std::vector<Slot<ir::Node>> slots;
    for (std::size_t index = 0; index < module.components.size(); ++index) {
        ir::Component& component = module.components[index];
        std::vector<Slot<ir::Node>> all;
        collect(component.root, component, component_prefix(module, index) + "/root[0]", all);
        for (Slot<ir::Node>& slot : all) {
            if (slot.node->ui_id.empty()) {
                slots.push_back(std::move(slot));
            }
        }
    }
    std::vector<automation::NodeIdInput> node_inputs;
    node_inputs.reserve(slots.size());
    for (const Slot<ir::Node>& slot : slots) {
        automation::NodeIdInput input;
        input.source_path = slot.component->source.file;
        input.node_path = slot.path;
        input.key = key_text(*slot.node);
        const std::string& text = slot.node->automation_id;
        if (!text.empty()) {
            std::uint64_t value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
                return std::unexpected(IdentityError{
                    "node `" + slot.node->id + "` has a non-numeric automationId `" + text + "`"});
            }
            input.automation_id = value;
        }
        node_inputs.push_back(std::move(input));
    }
    auto node_ids = automation::assign_ui_ids(salt, node_inputs);
    if (!node_ids) {
        return std::unexpected(IdentityError{describe(node_ids.error())});
    }

    for (std::size_t index = 0; index < module.components.size(); ++index) {
        if (module.components[index].component_id.empty()) {
            module.components[index].component_id = std::to_string((*component_ids)[index]);
        }
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
        slots[i].node->ui_id = std::to_string((*node_ids)[i].ui_id);
        // A Slot is a bare position marker with no scene node, so it carries no automation id
        // (the validator rejects one); it still has its `uiId`.
        if (mirror_automation_id && slots[i].node->kind != ir::NodeKind::Slot) {
            slots[i].node->automation_id = slots[i].node->ui_id;
        }
    }
    return {};
}

} // namespace

std::expected<void, IdentityError> assign_identity(ir::IrModule& module, std::string_view salt) {
    return assign_module_identity(module, salt, true);
}

std::expected<void, IdentityError> assign_library_identity(ir::IrModule& module) {
    return assign_module_identity(module, kLibraryIdSalt, false);
}

} // namespace tessera::compiler::ids
