// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/ir/source_span.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::ir {

// docs/specs/ir-v1.md section 3 and 3.4: the closed list of node kinds.
enum class NodeKind {
    View,
    Text,
    Button,
    TextInput,
    TextArea,
    Image,
    Path,
    Scroll,
    Spacer,
    VirtualList,
    VirtualGrid,
    ErrorBoundary,
    CustomElement,
    Component,
    Slot, // a component's `{children}` position (section 3.4); replaced by the instance's children
};

std::string_view node_kind_name(NodeKind kind);

// The loop is stored on the node it repeats (section 5); that node is the template.
struct Loop {
    std::string item;
    std::string index; // empty when the source has no index parameter
    ExprPtr source;
};

struct Node {
    NodeKind kind = NodeKind::View;
    std::string id;
    std::string ui_id;
    std::string automation_id; // empty when absent
    SourceSpan source;
    std::string element;   // CustomElement
    std::string component; // Component
    ExprPtr key;           // a literal, or an expression on a node with `loop`
    std::map<std::string, ExprPtr> props;
    std::map<std::string, ExprPtr> style;
    std::map<std::string, std::string> events; // event name -> bind:: or handler:: id
    ExprPtr text;
    std::vector<Node> children;
    ExprPtr condition;
    std::optional<Loop> loop;
    std::unique_ptr<Node> fallback; // ErrorBoundary
    // VirtualList/VirtualGrid: the static item template. It carries the `loop` (item/index names
    // and the data source) and the item `key`, but is realized lazily, not repeated eagerly
    // (section 3.5).
    std::unique_ptr<Node> item_template;
};

} // namespace tessera::compiler::ir
