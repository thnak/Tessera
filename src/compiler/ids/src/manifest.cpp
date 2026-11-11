// SPDX-License-Identifier: MIT

#include "node_walk.hpp"
#include "tessera/compiler/ids/manifest.hpp"
#include "tessera/compiler/ir/json_value.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::ids {

std::string write_manifest(const ir::IrModule& module, std::string_view salt) {
    ir::JsonValue components = ir::JsonValue::array();
    ir::JsonValue nodes = ir::JsonValue::array();
    for (const ir::Component& component : module.components) {
        ir::JsonValue entry = ir::JsonValue::object();
        entry.set("componentId", ir::JsonValue::string(component.component_id));
        entry.set("name", ir::JsonValue::string(component.name));
        entry.set("source", ir::JsonValue::string(component.source.file));
        components.push(std::move(entry));

        std::vector<Slot<const ir::Node>> walked;
        collect<const ir::Node>(component.root, component, component.name + "/root[0]", walked);
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
