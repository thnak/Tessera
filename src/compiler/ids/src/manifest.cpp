// SPDX-License-Identifier: MIT

#include "node_walk.hpp"
#include "tessera/compiler/ids/manifest.hpp"
#include "tessera/compiler/ir/json_value.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::ids {
namespace {

const std::string* literal_text(const ir::ExprPtr& expr) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return nullptr;
    }
    const auto* text = std::get_if<std::string>(&expr->literal);
    return text != nullptr && !text->empty() ? text : nullptr;
}

// The static accessible name: an explicit aria-label, else the `label` prop (button text), else the
// node's own literal text. Dynamic values are unknown to a harness that never runs the app, so they
// are omitted rather than guessed.
const std::string* static_name(const ir::Node& node) {
    for (const char* key : {"aria-label", "label"}) {
        if (const auto found = node.props.find(key); found != node.props.end()) {
            if (const std::string* text = literal_text(found->second); text != nullptr) {
                return text;
            }
            if (std::string_view(key) == "aria-label") {
                return nullptr; // a dynamic aria-label hides the fallbacks too
            }
        }
    }
    return literal_text(node.text);
}

} // namespace

std::string write_manifest(const ir::IrModule& module, std::string_view salt) {
    ir::JsonValue components = ir::JsonValue::array();
    ir::JsonValue nodes = ir::JsonValue::array();
    for (std::size_t index = 0; index < module.components.size(); ++index) {
        const ir::Component& component = module.components[index];
        ir::JsonValue entry = ir::JsonValue::object();
        entry.set("componentId", ir::JsonValue::string(component.component_id));
        entry.set("name", ir::JsonValue::string(component.name));
        entry.set("source", ir::JsonValue::string(component.source.file));
        components.push(std::move(entry));

        std::vector<Slot<const ir::Node>> walked;
        collect<const ir::Node>(component.root, component,
                                component_prefix(module, index) + "/root[0]", walked);
        for (const Slot<const ir::Node>& slot : walked) {
            ir::JsonValue source = ir::JsonValue::object();
            source.set("file", ir::JsonValue::string(slot.node->source.file));
            source.set("line", ir::JsonValue::integer(slot.node->source.line));
            source.set("column", ir::JsonValue::integer(slot.node->source.column));
            ir::JsonValue node = ir::JsonValue::object();
            node.set("uiId", ir::JsonValue::string(slot.node->ui_id));
            node.set("componentId", ir::JsonValue::string(component.component_id));
            node.set("kind",
                     ir::JsonValue::string(std::string(ir::node_kind_name(slot.node->kind))));
            if (const std::string* name = static_name(*slot.node); name != nullptr) {
                node.set("name", ir::JsonValue::string(*name));
            }
            node.set("path", ir::JsonValue::string(slot.path));
            node.set("source", std::move(source));
            nodes.push(std::move(node));
        }
    }
    ir::JsonValue document = ir::JsonValue::object();
    document.set("manifestVersion", ir::JsonValue::integer(kManifestVersion));
    document.set("projectSalt", ir::JsonValue::string(std::string(salt)));
    document.set("components", std::move(components));
    document.set("nodes", std::move(nodes));
    return ir::to_text(document);
}

} // namespace tessera::compiler::ids
