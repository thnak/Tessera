// SPDX-License-Identifier: MIT

// Generated header pieces for the dynamic child forms: the bounded item cap, the per-list item node
// structs, the component members (branch slots, group parent pointers, item tables) and the method
// declarations (docs/design/compiler.md section 11).

#include "emit_dynamic.hpp"
#include "emit_virtual.hpp"

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
    if (plan.has_virtual) {
        writer.line("// Hard cap of a generated collection geometry (virtualization-v1.md section");
        writer.line("// 10): a larger data source is refused loudly by scroll_virtual_<n>().");
        writer.line("inline constexpr std::size_t kMaxGeneratedVirtualGeometryItems = 4096;");
        writer.line(
            "// `onEndReached` throttle (virtualization-v1.md section 10): a re-entry into");
        writer.line("// the end zone within this scroll distance of the last fire is suppressed.");
        writer.line("inline constexpr float kVirtualEndReachedThrottlePx = 200.0F;");
    }
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
        if (list.is_virtual) {
            const std::string table = item_table_prefix(list);
            emit_virtual_geometry_fields(writer, list);
            writer.line(
                "// `node::" + ordinal_text(list.container_ordinal) +
                "` virtual item window (bounded; keyed reuse; the host applies the range).");
            writer.line("tessera::ui::Node* " + table + "_ = nullptr;");
            writer.line("std::vector<Item_" + ordinal + "> " + table + "_items_{};");
            writer.line("std::vector<std::uint64_t> " + table + "_hashes_{};");
            writer.line("std::vector<bool> " + table + "_set_{};");
            writer.line("std::vector<std::string> " + table + "_keys_{};");
            writer.line("std::size_t " + table + "_count_ = 0;");
            writer.line("std::size_t " + table + "_first_ = 0;");
            writer.line("std::uint64_t " + table + "_order_hash_ = 0;");
            writer.line("bool " + table + "_order_set_ = false;");
            writer.line(
                "// Geometry owned by the generated scroll handler; its count is synced from");
            writer.line("// the data source and update_virtual_geometry_<n>() re-seeds it on a");
            writer.line("// geometry change.");
            writer.line("tessera::ui::virtualization::ItemGeometry " + table + "_geometry_{{");
            writer.line("    kMaxGeneratedVirtualGeometryItems,");
            writer.line("    static_cast<float>(" + table + "_estimate_size_),");
            writer.line("    static_cast<float>(" + table + "_gap_)}};");
            writer.line("std::size_t " + table + "_geometry_count_ = 0;");
            writer.line("std::uint64_t " + table + "_geometry_hash_ = 0;");
            writer.line("bool " + table + "_geometry_set_ = false;");
            writer.line("std::uint64_t " + table + "_geometry_seeds_ = 0;");
            if (list.events.on_end_reached.present) {
                writer.line("// Edge/throttle state of the collection's `onEndReached`.");
                writer.line("bool " + table + "_end_armed_ = true;");
                writer.line("float " + table + "_end_fire_offset_ = 0.0f;");
                writer.line("std::uint64_t " + table + "_end_fires_ = 0;");
                writer.line("std::uint64_t " + table + "_end_throttled_ = 0;");
            }
            continue;
        }
        writer.line("// `node::" + ordinal + "` keyed list over `" + list.element_type +
                    "` items (bounded item table; reused by key).");
        writer.line("std::vector<Item_" + ordinal + "> list_" + ordinal + "_items_{};");
        writer.line("std::vector<std::uint64_t> list_" + ordinal + "_hashes_{};");
        writer.line("std::vector<bool> list_" + ordinal + "_set_{};");
        writer.line("std::vector<std::string> list_" + ordinal + "_keys_{};");
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
        if (list.is_virtual) {
            writer.line("// Applies the half-open realized window the host computed with");
            writer.line("// tessera::ui::virtualization::ItemGeometry; the table is reused by "
                        "key.");
            writer.line("void apply_virtual_" + ordinal_text(list.container_ordinal) +
                        "(std::size_t tessera_first, std::size_t tessera_last);");
            writer.line("// Re-seeds the geometry from the current geometry props/state and");
            writer.line("// re-anchors the offset so the item at the viewport start keeps its");
            writer.line("// place; true only when an input changed (hash-guarded).");
            writer.line("bool update_virtual_geometry_" + ordinal_text(list.container_ordinal) +
                        "(float& tessera_scroll, float tessera_viewport);");
            writer.line("// Applies one scroll offset: refines the geometry from realized item");
            writer.line("// measures (updating the offset in place on a re-anchor), applies the");
            writer.line("// window, then dispatches the collection events. False when the data");
            writer.line("// source exceeds the generated geometry cap.");
            writer.line("bool scroll_virtual_" + ordinal_text(list.container_ordinal) +
                        "(float& tessera_scroll, float tessera_viewport);");
            continue;
        }
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
