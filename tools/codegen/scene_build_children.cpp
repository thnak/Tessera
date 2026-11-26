// SPDX-License-Identifier: MIT

// A node's children in source order (issue #166; the two dynamic forms are issue #389).
//
// **Conditional children.** Lowering emits one sibling node per possible branch, each carrying a
// complementary `condition` (src/compiler/lowering/src/control_lowering.cpp): `{open ? <A/> :
// <B/>}` is two siblings, `{open && <A/>}` is one. So this walk visits every branch in source order
// and builds the ones whose condition the evaluator says is true. Two properties fall out of that
// shape rather than out of a rule invented here:
//
//   * **every branch is buildable**, not just the shown one. Both sibling nodes are ordinary IR
//     nodes; the preview builds the selected one now and would build the other for the other state,
//     because nothing about a branch is special-cased. An author toggling a cell therefore gets the
//     other branch, never a preview that broke on the branch it had not built.
//   * **only the shown branch exists in the tree**, which is what the app has: generated code keeps
//     both subtrees alive in branch slots and attaches one (emit_conditional.cpp), so the *painted*
//     child list is the same. Keep-alive state inside a hidden branch is a codegen property with no
//     scene-node counterpart, which is why the preview's snapshot and the native snapshot agree.
//
// **Repeated children.** A node carrying `loop` is the item template (ir-v1 section 5). Its items
// are the list `scene_build_dynamic.cpp` returns for the loop source; each item is built by the
// same `build_node()` with the item and index names bound in the scope, so a per-item expression
// sees the values a generated per-item builder would. Items are contiguous at the template's source
// position, in list order, exactly as `apply_list_<n>()` inserts them.

#include "scene_build_dynamic.hpp"
#include "scene_build_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli::internal {
namespace {

namespace detail = tessera::compiler::codegen::cli::detail;
namespace eval = tessera::compiler::eval;
namespace ir = tessera::compiler::ir;
namespace ui = tessera::ui;

// The loop-local names of one item, restored when the item's subtree is done: `item` and `index`
// are local refs (ir-v1 section 5), and a name that leaked past its template would resolve a later
// node against another item's value.
class LoopScope {
public:
    LoopScope(eval::Scope& scope, const ir::Loop& loop, eval::Value item, std::int32_t index)
        : scope_(&scope) {
        bind(loop.item, std::move(item));
        if (!loop.index.empty()) {
            bind(loop.index, eval::Value::of_int(index));
        }
    }
    ~LoopScope() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            if (it->second) {
                scope_->insert_or_assign(it->first, std::move(*it->second));
            } else {
                scope_->erase(it->first);
            }
        }
    }
    LoopScope(const LoopScope&) = delete;
    LoopScope& operator=(const LoopScope&) = delete;

private:
    void bind(const std::string& name, eval::Value value) {
        if (name.empty()) {
            return;
        }
        // A name the component scope already uses (a state cell called `item`) is put back after
        // the item, so a template cannot permanently shadow component state.
        auto found = scope_->find(name);
        if (found != scope_->end()) {
            saved_.emplace_back(name, std::move(found->second));
        } else {
            saved_.emplace_back(name, std::nullopt);
        }
        (*scope_)[name] = std::move(value);
    }

    eval::Scope* scope_;
    std::vector<std::pair<std::string, std::optional<eval::Value>>> saved_;
};

} // namespace

bool Builder::build_children(const ir::Node& node, ui::Node& parent, std::size_t depth) {
    for (const ir::Node& child : node.children) {
        if (child.condition) {
            if (!build_conditional_child(child, parent, depth)) {
                return false;
            }
            continue;
        }
        if (child.loop) {
            if (!build_repeated_child(child, parent, depth)) {
                return false;
            }
            continue;
        }
        if (!build_node(child, parent, depth + 1)) {
            return false;
        }
    }
    return true;
}

bool Builder::build_conditional_child(const ir::Node& child, ui::Node& parent, std::size_t depth) {
    bool show = false;
    if (!detail::evaluate_condition(evaluator_, *scope_, child, show, result_.error)) {
        return false;
    }
    if (!show) {
        // The sibling carrying the other branch is its own IR node and is visited in its own turn,
        // so a false condition is a branch that is simply not in the tree, not a subtree skipped
        // because it could not be built.
        return true;
    }
    return build_node(child, parent, depth + 1);
}

bool Builder::build_repeated_child(const ir::Node& child, ui::Node& parent, std::size_t depth) {
    if (loop_depth_ != 0) {
        // ir-v1 section 5 allows depth 2, and codegen implements one list level per template
        // (emit_dynamic_plan.cpp). Reporting the same limit keeps the preview predicting codegen
        // instead of showing a nested repetition the native build would refuse.
        return unsupported("a nested list template (`" + child.id +
                           "`) is deferred; v1 generates one list level per template");
    }
    eval::List items;
    if (!detail::evaluate_items(evaluator_, *scope_, child, items, result_.error)) {
        return false;
    }
    // A repeated child of a kind the preview does not build (a virtual collection) is reported
    // once, before the first item, rather than once per item. A repeated component instance is
    // built per item like any node: its props are evaluated with the item bound.
    if (child.kind == ir::NodeKind::CustomElement || child.kind == ir::NodeKind::VirtualList ||
        child.kind == ir::NodeKind::VirtualGrid || child.kind == ir::NodeKind::Slot) {
        return unsupported("a repeated `" + std::string(ir::node_kind_name(child.kind)) +
                           "` child (`" + child.id + "`) is not previewed yet");
    }

    const int saved_depth = loop_depth_;
    loop_depth_ = saved_depth + 1;
    bool ok = true;
    for (std::size_t index = 0; ok && index < items.size(); ++index) {
        const std::size_t before = parent.children().size();
        {
            // The key is evaluated *inside* the item scope: a key like `key={i}` or `key={row}` is
            // a loop-local reference (ir-v1 section 5), so it is only resolvable with the item
            // bound.
            LoopScope locals(*scope_, *child.loop, items[index], static_cast<std::int32_t>(index));
            bool present = false;
            std::uint64_t key = 0;
            if (!detail::evaluate_item_key(evaluator_, *scope_, child, present, key,
                                           result_.error)) {
                ok = false;
                break;
            }
            if (!build_node(child, parent, depth + 1)) {
                ok = false;
                break;
            }
            // The item root is where generated code publishes the numeric automation key
            // (emit_list.cpp), so the key is set on the node whose subtree was just built.
            if (present && parent.children().size() > before) {
                parent.children()[before]->set_item_key(key);
            }
        }
    }
    loop_depth_ = saved_depth;
    return ok;
}

} // namespace tessera::compiler::codegen::cli::internal
