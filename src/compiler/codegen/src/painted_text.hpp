// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/node.hpp"

namespace tessera::compiler::codegen::internal {

// The text a node paints, other than a controlled field's value: its `Text` payload, or a
// `Button`'s caption. A `Button` whose children are only text carries that text as `props.label`
// (elements-basic.md section 1, lowering decision 11), never as `node.text`, so a pass that looks
// at `node.text` alone leaves the caption unpainted: the generated base then built a button with no
// label (the scaffolded counter template drew two empty boxes). Every pass that decides what a
// node paints - the static payload, the dynamic update, the binding read set and the list-item
// content pass - asks this one function, so they cannot disagree.
[[nodiscard]] inline const ir::Expr* painted_text(const ir::Node& node) {
    if (node.text != nullptr) {
        return node.text.get();
    }
    if (node.kind != ir::NodeKind::Button) {
        return nullptr;
    }
    const auto label = node.props.find("label");
    return label == node.props.end() ? nullptr : label->second.get();
}

} // namespace tessera::compiler::codegen::internal
