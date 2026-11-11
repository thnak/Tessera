// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/node.hpp"

#include <string>
#include <vector>

namespace tessera::compiler::ids {

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
