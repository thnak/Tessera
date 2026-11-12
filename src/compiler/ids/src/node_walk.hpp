// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/node.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tessera::compiler::ids {

// The path prefix of the component at `index` (docs/specs/automation-ids-v1.md section 2.1): its
// name, plus `#<occurrence>` for the second and later component with that name in the same source
// file. The first occurrence keeps the bare name, so ordinary components keep the paths (and thus
// the ids) they always had. Both the id assignment and the manifest walk use this, so a path is
// hashed and listed identically.
inline std::string component_prefix(const ir::IrModule& module, std::size_t index) {
    const ir::Component& component = module.components[index];
    std::size_t occurrence = 0;
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
        const ir::Component& candidate = module.components[earlier];
        if (candidate.name == component.name && candidate.source.file == component.source.file) {
            ++occurrence;
        }
    }
    std::string prefix = component.name;
    if (occurrence != 0) {
        prefix += '#';
        prefix += std::to_string(occurrence);
    }
    return prefix;
}

// A node with its structural path (`Counter/root[0]/Button[1]`, a fallback or item template adds
// `fallback[0]`/`template[0]`). The id assignment and the manifest walk the tree the same way, so
// the path a node is hashed with is the path the manifest lists.
// `NodeT` is `ir::Node` to assign ids and `const ir::Node` to read them.
template<typename NodeT> struct Slot {
    NodeT* node = nullptr;
    const ir::Component* component = nullptr;
    std::string path;
};

template<typename NodeT>
void collect(NodeT& node, const ir::Component& component, const std::string& path,
             std::vector<Slot<NodeT>>& out) {
    out.push_back({&node, &component, path});
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        NodeT& child = node.children[i];
        collect(child, component,
                path + "/" + std::string(ir::node_kind_name(child.kind)) + "[" + std::to_string(i) +
                    "]",
                out);
    }
    if (node.fallback) {
        collect<NodeT>(*node.fallback, component, path + "/fallback[0]", out);
    }
    if (node.item_template) {
        collect<NodeT>(*node.item_template, component, path + "/template[0]", out);
    }
}

} // namespace tessera::compiler::ids
