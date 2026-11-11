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

std::expected<void, IdentityError> assign_identity(ir::IrModule& module, std::string_view salt) {
    if (salt.empty()) {
        return std::unexpected(
            IdentityError{"the project salt (tessera.json `namespace`) is empty"});
    }

    // Components first, in module order.
    std::vector<ir::Component*> pending_components;
    std::vector<automation::ComponentIdInput> component_inputs;
    for (ir::Component& component : module.components) {
        if (component.component_id.empty()) {
            pending_components.push_back(&component);
            component_inputs.push_back({component.source.file, component.name});
        }
    }
    auto component_ids = automation::assign_component_ids(salt, component_inputs);
    if (!component_ids) {
        return std::unexpected(IdentityError{describe(component_ids.error())});
    }

    // Then the nodes that have no uiId yet, in document order per component.
    std::vector<Slot<ir::Node>> slots;
    for (ir::Component& component : module.components) {
        std::vector<Slot<ir::Node>> all;
        collect(component.root, component, component.name + "/root[0]", all);
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

    for (std::size_t i = 0; i < pending_components.size(); ++i) {
        pending_components[i]->component_id = std::to_string((*component_ids)[i]);
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
        slots[i].node->ui_id = std::to_string((*node_ids)[i].ui_id);
        slots[i].node->automation_id = slots[i].node->ui_id;
    }
    return {};
}

} // namespace tessera::compiler::ids
