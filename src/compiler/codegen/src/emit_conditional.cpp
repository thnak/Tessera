// SPDX-License-Identifier: MIT

// Generated `<Name>::apply_children_<p>()`: one deterministic walk over a parent's child slots that
// attaches/detaches conditional branches at their source position (docs/design/compiler.md section
// 11). Static children are attached by build() and only advance the position; a list template
// advances it by its current item count.

#include "emit_dynamic.hpp"
#include "value_ops.hpp"

#include <string>
#include <utility>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

// `a > b` -> `(a > b)`: the emitted condition is used as a bool initializer.
std::string parenthesized(const std::string& text) {
    if (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
        return text;
    }
    return "(" + text + ")";
}

// One deterministic child-set walk: the component-level `apply_children_<p>()` and the per-item
// `apply_item_children_<p>()` differ only in where the parent pointer and the branch slots live
// (docs/design/compiler-node-state.md section 4).
struct WalkTarget {
    std::string head;          // the emitted function signature head
    std::string parent_check;  // statement(s) that bind `parent` or return
    std::string parent;        // the parent lvalue (`parent` or `*tessera_out.group_parent_7`)
    std::string branch_prefix; // `branch_` (members) or `tessera_out.branch_` (item record)
    // Component members end with an underscore (`branch_3_owned_`); item-record fields do not
    // (`branch_3_owned`).
    std::string field_suffix;
    // Statements that mark the template parameters used, so an item walk whose condition reads only
    // the component scope does not trip -Wunused-parameter.
    std::string void_params;
    LocalBindings locals; // loop locals visible to the condition expressions
    // A per-item walk: a list child inside a list template would be a nested template, which the
    // plan already rejects, so reaching one here is an internal error.
    bool item = false;
};

bool emit_walk(CppWriter& writer, const ir::Component& component, const DynamicGroup& group,
               const WalkTarget& target, std::string& error) {
    writer.open(target.head);
    writer.line(target.parent_check);
    if (!target.void_params.empty()) {
        std::size_t start = 0;
        while (start <= target.void_params.size()) {
            const std::size_t end = target.void_params.find('\n', start);
            const std::size_t stop = end == std::string::npos ? target.void_params.size() : end;
            writer.line(std::string_view(target.void_params).substr(start, stop - start));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    writer.line("tessera::ui::Node& parent = " + target.parent + ";");
    writer.line("std::size_t position = 0;");
    for (const DynamicChild& child : group.children) {
        switch (child.kind) {
        case DynamicChildKind::Static:
            writer.line("++position; // a static child is attached by build()");
            break;
        case DynamicChildKind::List: {
            if (target.item) {
                return fail(error, "a nested list template inside a list template is deferred");
            }
            const std::string list_ordinal = std::to_string(child.ordinal);
            writer.line("apply_list_" + list_ordinal + "(parent, position);");
            writer.line("position += list_" + list_ordinal + "_count_;");
            break;
        }
        case DynamicChildKind::Conditional: {
            if (child.node == nullptr || !child.node->condition) {
                return fail(error, "conditional branch `node::" + std::to_string(child.ordinal) +
                                       "` has no condition expression");
            }
            const std::string branch = std::to_string(child.ordinal);
            const std::string prefix = target.branch_prefix + branch;
            const std::string owned = prefix + "_owned" + target.field_suffix;
            const std::string node_field = prefix + "_node" + target.field_suffix;
            const std::string hash_field = prefix + "_hash" + target.field_suffix;
            const std::string set_field = prefix + "_set" + target.field_suffix;
            const std::string present_field = prefix + "_present" + target.field_suffix;
            const std::string hash = "branch_" + branch + "_hash";
            const std::string show = "branch_" + branch + "_show";
            writer.line("{");
            ++writer.indent;
            writer.line("std::uint64_t " + hash + " = tessera::ui::kEffectHashSeed;");
            if (!emit_value_ops(writer, *child.node->condition, component, target.locals, hash, "",
                                error)) {
                return false;
            }
            const ExprEmission condition =
                emit_expr(*child.node->condition, component, target.locals);
            if (!condition.error.empty()) {
                return fail(error, "the condition of `" + child.node->id + "`: " + condition.error);
            }
            writer.line("const bool " + show + " = " + parenthesized(condition.text) + ";");
            // Hash first: an unchanged condition and presence touch no node API at all.
            writer.open("if (!" + set_field + " || " + hash + " != " + hash_field + " || " + show +
                        " != " + present_field + ")");
            writer.line(hash_field + " = " + hash + ";");
            writer.line(set_field + " = true;");
            writer.line(present_field + " = " + show + ";");
            writer.open("if (" + show + ")");
            writer.line("attach_child_at(parent, position, " + owned + ", " + node_field + ");");
            writer.close("");
            writer.line("else {");
            ++writer.indent;
            writer.line("detach_child(parent, " + owned + ", " + node_field + ");");
            --writer.indent;
            writer.line("}");
            writer.close("");
            writer.open("if (" + show + ")");
            writer.line("++position;");
            writer.close("");
            --writer.indent;
            writer.line("}");
            break;
        }
        }
    }
    writer.close("");
    writer.line("");
    return true;
}

} // namespace

bool emit_apply_children(CppWriter& writer, const ir::Component& component, const DynamicPlan& plan,
                         const DynamicGroup& group, std::string& error) {
    (void)plan;
    const std::string ordinal = std::to_string(group.parent_ordinal);
    WalkTarget target;
    target.head = "void " + component.name + "::apply_children_" + ordinal + "()";
    target.parent_check = "if (children_parent_" + ordinal + "_ == nullptr) { return; }";
    target.parent = "*children_parent_" + ordinal + "_";
    target.branch_prefix = "branch_";
    target.field_suffix = "_";
    return emit_walk(writer, component, group, target, error);
}

bool emit_item_apply_children(CppWriter& writer, const ir::Component& component,
                              const DynamicPlan& plan, const DynamicGroup& group,
                              std::string& error) {
    const ListTemplate* owner = nullptr;
    for (const ListTemplate& list : plan.lists) {
        if (list.ordinal == group.template_ordinal) {
            owner = &list;
            break;
        }
    }
    if (owner == nullptr) {
        return fail(error, "a dynamic child set inside a template has no owning list template");
    }
    const std::string ordinal = std::to_string(group.parent_ordinal);
    WalkTarget target;
    target.head = "void " + component.name + "::apply_item_children_" + ordinal + "(Item_" +
                  std::to_string(owner->ordinal) + "& tessera_out, const " + owner->element_cpp +
                  "& " + owner->item + ", std::size_t tessera_index)";
    target.parent_check = "if (tessera_out.group_parent_" + ordinal + " == nullptr) { return; }";
    target.parent = "*tessera_out.group_parent_" + ordinal;
    target.branch_prefix = "tessera_out.branch_";
    target.item = true;
    target.void_params = "(void)" + owner->item + ";\n(void)tessera_index;";
    target.locals.push_back(LocalBinding{owner->item, owner->item, owner->element_type});
    if (!owner->index.empty()) {
        target.locals.push_back(
            LocalBinding{owner->index, "static_cast<int>(tessera_index)", "int"});
    }
    return emit_walk(writer, component, group, target, error);
}

} // namespace tessera::compiler::codegen::internal
