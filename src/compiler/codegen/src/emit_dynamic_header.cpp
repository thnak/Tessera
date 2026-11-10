// SPDX-License-Identifier: MIT

// Generated header pieces for the dynamic child forms: the bounded item cap, the per-list item node
// structs, the component members (branch slots, group parent pointers, item tables) and the method
// declarations (docs/design/compiler.md section 11).

#include "emit_dynamic.hpp"

#include <string>

namespace tessera::compiler::codegen::internal {
namespace {

std::string ordinal_text(unsigned ordinal) {
    return std::to_string(ordinal);
}

} // namespace

void emit_dynamic_item_structs(CppWriter& writer, const DynamicPlan& plan) {
    if (plan.lists.empty()) {
        return;
    }
    writer.line("// Bound of the generated keyed-list item table (docs/design/compiler.md section");
    writer.line("// 11): a list materializes at most this many items; a longer source list is");
    writer.line("// truncated at this prefix deterministically.");
    writer.line("inline constexpr std::size_t kMaxGeneratedListItems = 32;");
    writer.line("");
    for (const ListTemplate& list : plan.lists) {
        writer.line("// Generated nodes of one `node::" + ordinal_text(list.ordinal) + "` item (" +
                    std::to_string(list.text_sites.size()) +
                    " interpolated text node(s), template order).");
        writer.line("struct Item_" + ordinal_text(list.ordinal) + " {");
        ++writer.indent;
        writer.line("tessera::ui::Node* root = nullptr;");
        writer.line("std::size_t id_slot = 0; // NodeId offset slot; stable while the item lives");
        writer.line("std::array<tessera::ui::Node*, " + std::to_string(list.text_sites.size()) +
                    "> texts{};");
        --writer.indent;
        writer.line("};");
        writer.line("");
    }
}

void emit_dynamic_fields(CppWriter& writer, const DynamicPlan& plan) {
    for (const DynamicGroup& group : plan.groups) {
        for (const DynamicChild& child : group.children) {
            if (child.kind != DynamicChildKind::Conditional) {
                continue;
            }
            const std::string ordinal = ordinal_text(child.ordinal);
            writer.line("// `node::" + ordinal +
                        "` conditional branch (attached at its source "
                        "position while the condition holds).");
            writer.line("std::unique_ptr<tessera::ui::Node> branch_" + ordinal + "_owned_;");
            writer.line("tessera::ui::Node* branch_" + ordinal + "_node_ = nullptr;");
            writer.line("std::uint64_t branch_" + ordinal + "_hash_ = 0;");
            writer.line("bool branch_" + ordinal + "_set_ = false;");
            writer.line("bool branch_" + ordinal + "_present_ = false;");
        }
        writer.line("// Dynamic child set of `node::" + ordinal_text(group.parent_ordinal) + "`.");
        writer.line("tessera::ui::Node* children_parent_" + ordinal_text(group.parent_ordinal) +
                    "_ = nullptr;");
    }
    for (const ListTemplate& list : plan.lists) {
        const std::string ordinal = ordinal_text(list.ordinal);
        writer.line("// `node::" + ordinal + "` keyed list over `" + list.element_type +
                    "` items (bounded item table; reused by key).");
        writer.line("std::array<Item_" + ordinal + ", kMaxGeneratedListItems> list_" + ordinal +
                    "_items_{};");
        writer.line("std::array<std::uint64_t, kMaxGeneratedListItems> list_" + ordinal +
                    "_hashes_{};");
        writer.line("std::array<bool, kMaxGeneratedListItems> list_" + ordinal + "_set_{};");
        writer.line("std::array<std::string, kMaxGeneratedListItems> list_" + ordinal +
                    "_keys_{};");
        writer.line("std::size_t list_" + ordinal + "_count_ = 0;");
        writer.line("std::uint64_t list_" + ordinal + "_order_hash_ = 0;");
        writer.line("bool list_" + ordinal + "_order_set_ = false;");
    }
}

void emit_dynamic_declarations(CppWriter& writer, const DynamicPlan& plan) {
    writer.line("// Generated dynamic child paths: deterministic child positions and bounded keyed "
                "lists.");
    for (const DynamicGroup& group : plan.groups) {
        writer.line("void apply_children_" + ordinal_text(group.parent_ordinal) + "();");
    }
    for (const ListTemplate& list : plan.lists) {
        const std::string ordinal = ordinal_text(list.ordinal);
        writer.line("std::unique_ptr<tessera::ui::Node> build_item_" + ordinal + "(const " +
                    list.element_cpp + "& " + list.item +
                    ", std::size_t tessera_index, std::size_t tessera_id_slot, Item_" + ordinal +
                    "& tessera_out);");
        writer.line("void apply_item_" + ordinal + "(std::size_t tessera_slot, const " +
                    list.element_cpp + "& " + list.item + ", std::size_t tessera_index);");
        writer.line("void apply_list_" + ordinal +
                    "(tessera::ui::Node& parent, std::size_t insert_at);");
    }
}

void emit_dynamic_helpers(CppWriter& writer, const DynamicPlan& plan) {
    if (plan.groups.empty() && plan.lists.empty()) {
        return;
    }
    writer.open("namespace");
    if (plan.has_lists) {
        // Item node ids: the template's pre-order ids offset by the item index, so one template
        // producing many instances keeps every generated NodeId unique (damage tracking and
        // automation key on it). One past the last pre-order id is the stride.
        writer.line("// Item nodes of a list template use `template id + item index * "
                    "kItemIdStride`.");
        writer.line("constexpr std::uint64_t kItemIdStride = " +
                    std::to_string(plan.max_ordinal + 1) + "ull;");
        writer.line("");
    }
    if (plan.has_conditionals) {
        // A conditional branch lives in its ownership slot while detached and in the parent while
        // attached; `live` is non-null only in the attached state, so a repeated update is a no-op.
        writer.line("// Attach (or reposition) a conditional branch at its deterministic child "
                    "position.");
        writer.line("void attach_child_at(tessera::ui::Node& parent, std::size_t index,");
        writer.line("                     std::unique_ptr<tessera::ui::Node>& slot,");
        writer.line("                     tessera::ui::Node*& live) {");
        ++writer.indent;
        writer.line("if (live == nullptr) {");
        ++writer.indent;
        writer.line("if (!slot) {");
        ++writer.indent;
        writer.line("return;");
        --writer.indent;
        writer.line("}");
        writer.line("const std::size_t at =");
        writer.line("    index < parent.children().size() ? index : parent.children().size();");
        writer.line("parent.insert_child(at, std::move(slot));");
        writer.line("live = parent.children()[at].get();");
        writer.line("return;");
        --writer.indent;
        writer.line("}");
        writer.line(
            "if (index < parent.children().size() && parent.children()[index].get() == live) {");
        ++writer.indent;
        writer.line("return;");
        --writer.indent;
        writer.line("}");
        writer.line("std::unique_ptr<tessera::ui::Node> moved = parent.remove_child(*live);");
        writer.line("parent.insert_child(index, std::move(moved));");
        --writer.indent;
        writer.line("}");
        writer.line("");
        writer.line(
            "// Detach a conditional branch, keeping its subtree alive for a later re-attach.");
        writer.line("void detach_child(tessera::ui::Node& parent,");
        writer.line("                  std::unique_ptr<tessera::ui::Node>& slot,");
        writer.line("                  tessera::ui::Node*& live) {");
        ++writer.indent;
        writer.line("if (live == nullptr) {");
        ++writer.indent;
        writer.line("return;");
        --writer.indent;
        writer.line("}");
        writer.line("slot = parent.remove_child(*live);");
        writer.line("live = nullptr;");
        --writer.indent;
        writer.line("}");
        writer.line("");
    }
    writer.close("");
    writer.line("");
}

} // namespace tessera::compiler::codegen::internal
