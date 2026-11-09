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

} // namespace

bool emit_apply_children(CppWriter& writer, const ir::Component& component, const DynamicPlan& plan,
                         const DynamicGroup& group, std::string& error) {
    (void)plan;
    const std::string ordinal = std::to_string(group.parent_ordinal);
    writer.open("void " + component.name + "::apply_children_" + ordinal + "()");
    writer.open("if (children_parent_" + ordinal + "_ == nullptr)");
    writer.line("return;");
    writer.close("");
    writer.line("tessera::ui::Node& parent = *children_parent_" + ordinal + "_;");
    writer.line("std::size_t position = 0;");
    for (const DynamicChild& child : group.children) {
        switch (child.kind) {
        case DynamicChildKind::Static:
            writer.line("++position; // a static child is attached by build()");
            break;
        case DynamicChildKind::List: {
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
            const std::string hash = "branch_" + branch + "_hash";
            const std::string show = "branch_" + branch + "_show";
            writer.line("{");
            ++writer.indent;
            writer.line("std::uint64_t " + hash + " = tessera::ui::kEffectHashSeed;");
            if (!emit_value_ops(writer, *child.node->condition, component, {}, hash, "", error)) {
                return false;
            }
            const ExprEmission condition = emit_expr(*child.node->condition, component);
            if (!condition.error.empty()) {
                return fail(error, "the condition of `" + child.node->id + "`: " + condition.error);
            }
            writer.line("const bool " + show + " = " + parenthesized(condition.text) + ";");
            // Hash first: an unchanged condition and presence touch no node API at all.
            writer.open("if (!branch_" + branch + "_set_ || " + hash + " != branch_" + branch +
                        "_hash_ || " + show + " != branch_" + branch + "_present_)");
            writer.line("branch_" + branch + "_hash_ = " + hash + ";");
            writer.line("branch_" + branch + "_set_ = true;");
            writer.line("branch_" + branch + "_present_ = " + show + ";");
            writer.open("if (" + show + ")");
            writer.line("attach_child_at(parent, position, branch_" + branch + "_owned_, branch_" +
                        branch + "_node_);");
            writer.close("");
            writer.line("else {");
            ++writer.indent;
            writer.line("detach_child(parent, branch_" + branch + "_owned_, branch_" + branch +
                        "_node_);");
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

} // namespace tessera::compiler::codegen::internal
