// SPDX-License-Identifier: MIT

// Generated keyed-list emission (docs/design/compiler.md section 11):
//
//   build_item_<n>(item, index, out)  builds one template instance and records its interpolated
//                                     text nodes; node ids are offset by the item index so every
//                                     instance has a unique NodeId;
//   apply_item_<n>(slot, item, index) hash-compares the item's interpolated content and repaints
//                                     only when it changed (no allocation on an unchanged item);
//   apply_list_<n>(parent, at)        hashes the key list first (no allocation when unchanged),
//                                     reconciles the bounded item table by key
//                                     (reuse/insert/remove), then runs the per-item content pass.

#include "emit_dynamic.hpp"
#include "emit_node.hpp"
#include "emit_virtual.hpp"
#include "value_ops.hpp"

#include <string>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

// The loop locals of one list template for the emitted C++ statements: the item is the range-for
// variable, the index is the generated position counter cast to the IR `int` type.
LocalBindings list_locals(const ListTemplate& list, std::string_view index_cpp) {
    LocalBindings locals;
    locals.push_back(LocalBinding{list.item, list.item, list.element_type});
    if (!list.index.empty()) {
        locals.push_back(LocalBinding{list.index, std::string(index_cpp), "int"});
    }
    return locals;
}

bool emit_build_item(CppWriter& writer, const ir::Component& component, const DynamicPlan& plan,
                     const ListTemplate& list, const std::vector<EventAdapter>& adapters,
                     const std::vector<DynamicText>& dynamic_text,
                     const std::vector<DynamicMetadata>& dynamic_metadata,
                     const std::vector<ControlledInput>& controlled_inputs, std::string& error) {
    const std::string ordinal = std::to_string(list.ordinal);
    writer.open("std::unique_ptr<tessera::ui::Node> " + component.name + "::build_item_" + ordinal +
                "(const " + list.element_cpp + "& " + list.item +
                ", std::size_t tessera_index, std::size_t tessera_id_slot, Item_" + ordinal +
                "& tessera_out)");
    writer.line("(void)" + list.item + ";");
    writer.line("(void)tessera_index;");
    writer.line("(void)tessera_out;");
    // The NodeId offset follows the item, not its position: a reused item keeps its ids and a new
    // item takes a slot no live item holds, so ids stay unique across reorders and removals.
    writer.line("tessera_out.id_slot = tessera_id_slot;");
    const LocalBindings locals = list_locals(list, "static_cast<int>(tessera_index)");
    NodeEmitState state{component,        adapters,          dynamic_text,
                        dynamic_metadata, controlled_inputs, &plan};
    state.locals = &locals;
    state.text_sites = &list.text_sites;
    state.index_expr = "tessera_id_slot";
    state.text_out = "tessera_out.texts";
    state.next_id = list.ordinal;
    state.next_var = list.ordinal;
    std::string root;
    if (!emit_node(writer, *list.node, state, root, error)) {
        return false;
    }
    // No std::move: the returned unique_ptr is a local, and GCC's -Wpessimizing-move (CI builds
    // with -Werror) rejects a move that prevents copy elision.
    writer.line("return " + root + ";");
    writer.close("");
    writer.line("");
    return true;
}

bool emit_apply_item(CppWriter& writer, const ir::Component& component, const ListTemplate& list,
                     std::string& error) {
    const std::string ordinal = std::to_string(list.ordinal);
    const std::string table = item_table_prefix(list);
    writer.open("void " + component.name + "::apply_item_" + ordinal +
                "(std::size_t tessera_slot, const " + list.element_cpp + "& " + list.item +
                ", std::size_t tessera_index)");
    writer.line("(void)" + list.item + ";");
    writer.line("(void)tessera_index;");
    const LocalBindings locals = list_locals(list, "static_cast<int>(tessera_index)");
    writer.line("std::uint64_t tessera_hash = tessera::ui::kEffectHashSeed;");
    for (const ListTextSite& site : list.text_sites) {
        if (!emit_value_ops(writer, *site.text, component, locals, "tessera_hash", "", error)) {
            return false;
        }
    }
    writer.open("if (!" + table + "_set_[tessera_slot] || tessera_hash != " + table +
                "_hashes_[tessera_slot])");
    writer.line(table + "_set_[tessera_slot] = true;");
    writer.line(table + "_hashes_[tessera_slot] = tessera_hash;");
    for (const ListTextSite& site : list.text_sites) {
        const std::string site_text = std::to_string(site.site);
        writer.line("std::string tessera_text_" + site_text + ";");
        if (!emit_value_ops(writer, *site.text, component, locals, "", "tessera_text_" + site_text,
                            error)) {
            return false;
        }
        writer.line("tessera::ui::PaintData tessera_paint_" + site_text + ";");
        writer.line("tessera_paint_" + site_text + ".text = std::move(tessera_text_" + site_text +
                    ");");
        writer.line(table + "_items_[tessera_slot].texts[" + site_text +
                    "]->set_paint_data(std::move(tessera_paint_" + site_text + "));");
    }
    writer.close("");
    writer.close("");
    writer.line("");
    return true;
}

bool emit_apply_list(CppWriter& writer, const ir::Component& component, const ListTemplate& list,
                     std::string& error) {
    const std::string ordinal = std::to_string(list.ordinal);
    writer.open("void " + component.name + "::apply_list_" + ordinal +
                "(tessera::ui::Node& parent, std::size_t insert_at)");
    const ExprEmission source = emit_expr(*list.node->loop->source, component);
    if (!source.error.empty()) {
        return fail(error, "the source of `" + list.node->id + "`: " + source.error);
    }
    const std::string key_locals_cpp = "static_cast<int>(tessera_position)";
    const LocalBindings key_locals = list_locals(list, key_locals_cpp);
    const LocalBindings next_locals = list_locals(list, "static_cast<int>(tessera_next)");
    writer.line("// Key/order hash first: an unchanged key list allocates nothing and touches no "
                "node.");
    writer.line("std::uint64_t tessera_order = tessera::ui::kEffectHashSeed;");
    writer.line("std::size_t tessera_position = 0;");
    writer.open("for (const " + list.element_cpp + "& " + list.item + " : " + source.text + ")");
    writer.open("if (tessera_position == kMaxGeneratedListItems)");
    writer.line("break;");
    writer.close("");
    if (!emit_value_ops(writer, *list.node->key, component, key_locals, "tessera_order", "",
                        error)) {
        return false;
    }
    writer.line("++tessera_position;");
    writer.close("");
    writer.line("tessera_order = tessera::ui::effect_hash(tessera_order, "
                "static_cast<int>(tessera_position));");
    writer.open("if (!list_" + ordinal + "_order_set_ || tessera_order != list_" + ordinal +
                "_order_hash_)");
    writer.line("list_" + ordinal + "_order_set_ = true;");
    writer.line("list_" + ordinal + "_order_hash_ = tessera_order;");
    writer.line("// Detach the current items, keeping their subtrees and per-item content state.");
    writer.line("std::array<std::unique_ptr<tessera::ui::Node>, kMaxGeneratedListItems> "
                "tessera_old_nodes{};");
    writer.line("std::array<Item_" + ordinal + ", kMaxGeneratedListItems> tessera_old_items{};");
    writer.line("std::array<std::uint64_t, kMaxGeneratedListItems> tessera_old_hashes{};");
    writer.line("std::array<bool, kMaxGeneratedListItems> tessera_old_set{};");
    writer.open("for (std::size_t tessera_slot = 0; tessera_slot < list_" + ordinal +
                "_count_; ++tessera_slot)");
    writer.line("tessera_old_nodes[tessera_slot] = "
                "parent.remove_child(*list_" +
                ordinal + "_items_[tessera_slot].root);");
    writer.line("tessera_old_items[tessera_slot] = list_" + ordinal + "_items_[tessera_slot];");
    writer.line("tessera_old_hashes[tessera_slot] = list_" + ordinal + "_hashes_[tessera_slot];");
    writer.line("tessera_old_set[tessera_slot] = list_" + ordinal + "_set_[tessera_slot];");
    writer.close("");
    writer.line("std::array<bool, kMaxGeneratedListItems> tessera_reused{};");
    writer.line("std::array<std::string, kMaxGeneratedListItems> tessera_next_keys{};");
    writer.line("std::array<Item_" + ordinal + ", kMaxGeneratedListItems> tessera_next_items{};");
    writer.line("std::array<std::uint64_t, kMaxGeneratedListItems> tessera_next_hashes{};");
    writer.line("std::array<bool, kMaxGeneratedListItems> tessera_next_set{};");
    writer.line("std::size_t tessera_next = 0;");
    writer.line("// Id slots held by any item of the previous table; a new item takes a free one.");
    writer.line("std::array<bool, 2 * kMaxGeneratedListItems> tessera_id_used{};");
    writer.open("for (std::size_t tessera_slot = 0; tessera_slot < list_" + ordinal +
                "_count_; ++tessera_slot)");
    writer.line("tessera_id_used[tessera_old_items[tessera_slot].id_slot] = true;");
    writer.close("");
    writer.open("for (const " + list.element_cpp + "& " + list.item + " : " + source.text + ")");
    writer.open("if (tessera_next == kMaxGeneratedListItems)");
    writer.line("break;");
    writer.close("");
    const ExprEmission key = emit_expr(*list.node->key, component, next_locals);
    if (!key.error.empty()) {
        return fail(error, "the key of `" + list.node->id + "`: " + key.error);
    }
    writer.line("std::string tessera_key = format_value(" + key.text + ");");
    writer.line("std::size_t tessera_match = kMaxGeneratedListItems;");
    writer.open("for (std::size_t tessera_slot = 0; tessera_slot < list_" + ordinal +
                "_count_; ++tessera_slot)");
    writer.open("if (!tessera_reused[tessera_slot] && list_" + ordinal +
                "_keys_[tessera_slot] == tessera_key)");
    writer.line("tessera_reused[tessera_slot] = true;");
    writer.line("tessera_match = tessera_slot;");
    writer.line("break;");
    writer.close("");
    writer.close("");
    writer.open("if (tessera_match != kMaxGeneratedListItems)");
    writer.line("// Reuse: the node subtree, its interpolated text pointers and its content hash "
                "move to the new slot.");
    writer.line("tessera_next_items[tessera_next] = tessera_old_items[tessera_match];");
    writer.line("tessera_next_hashes[tessera_next] = tessera_old_hashes[tessera_match];");
    writer.line("tessera_next_set[tessera_next] = tessera_old_set[tessera_match];");
    writer.line("parent.insert_child(insert_at + tessera_next, "
                "std::move(tessera_old_nodes[tessera_match]));");
    writer.close("");
    writer.line("else {");
    ++writer.indent;
    writer.line("std::size_t tessera_id_slot = 0;");
    writer.line("while (tessera_id_used[tessera_id_slot]) {");
    writer.line("    ++tessera_id_slot;");
    writer.line("}");
    writer.line("tessera_id_used[tessera_id_slot] = true;");
    writer.line("std::unique_ptr<tessera::ui::Node> tessera_built = build_item_" + ordinal + "(" +
                list.item + ", tessera_next, tessera_id_slot, tessera_next_items[tessera_next]);");
    writer.line("tessera_next_items[tessera_next].root = tessera_built.get();");
    writer.line("tessera_next_hashes[tessera_next] = 0;");
    writer.line("tessera_next_set[tessera_next] = false;");
    writer.line("parent.insert_child(insert_at + tessera_next, std::move(tessera_built));");
    --writer.indent;
    writer.line("}");
    writer.line("tessera_next_keys[tessera_next] = std::move(tessera_key);");
    writer.line("++tessera_next;");
    writer.close("");
    writer.line(
        "// Publish the new table; the stale slots are cleared so a later reuse cannot match "
        "a removed key.");
    writer.open("for (std::size_t tessera_slot = 0; tessera_slot < tessera_next; ++tessera_slot)");
    writer.line("list_" + ordinal + "_items_[tessera_slot] = tessera_next_items[tessera_slot];");
    writer.line("list_" + ordinal + "_hashes_[tessera_slot] = tessera_next_hashes[tessera_slot];");
    writer.line("list_" + ordinal + "_set_[tessera_slot] = tessera_next_set[tessera_slot];");
    writer.line("list_" + ordinal +
                "_keys_[tessera_slot] = std::move(tessera_next_keys["
                "tessera_slot]);");
    writer.close("");
    writer.open("for (std::size_t tessera_slot = tessera_next; tessera_slot < list_" + ordinal +
                "_count_; ++tessera_slot)");
    writer.line("list_" + ordinal + "_items_[tessera_slot] = Item_" + ordinal + "{};");
    writer.line("list_" + ordinal + "_hashes_[tessera_slot] = 0;");
    writer.line("list_" + ordinal + "_set_[tessera_slot] = false;");
    writer.line("list_" + ordinal + "_keys_[tessera_slot].clear();");
    writer.close("");
    writer.line("list_" + ordinal + "_count_ = tessera_next;");
    writer.close("");
    writer.line(
        "// Content pass: hash-compared per item, so an unchanged item marks nothing dirty.");
    writer.line("std::size_t tessera_slot = 0;");
    writer.open("for (const " + list.element_cpp + "& " + list.item + " : " + source.text + ")");
    writer.open("if (tessera_slot == list_" + ordinal + "_count_)");
    writer.line("break;");
    writer.close("");
    writer.line("apply_item_" + ordinal + "(tessera_slot, " + list.item + ", tessera_slot);");
    writer.line("++tessera_slot;");
    writer.close("");
    writer.close("");
    writer.line("");
    return true;
}

} // namespace

bool emit_list_sources(CppWriter& writer, const ir::Component& component, const DynamicPlan& plan,
                       const std::vector<EventAdapter>& adapters,
                       const std::vector<DynamicText>& dynamic_text,
                       const std::vector<DynamicMetadata>& dynamic_metadata,
                       const std::vector<ControlledInput>& controlled_inputs, std::string& error) {
    for (const ListTemplate& list : plan.lists) {
        if (!emit_build_item(writer, component, plan, list, adapters, dynamic_text,
                             dynamic_metadata, controlled_inputs, error)) {
            return false;
        }
        if (!emit_apply_item(writer, component, list, error)) {
            return false;
        }
        if (list.is_virtual) {
            if (!emit_apply_virtual(writer, component, list, error)) {
                return false;
            }
            if (!emit_virtual_scroll(writer, component, list, error)) {
                return false;
            }
            continue;
        }
        if (!emit_apply_list(writer, component, list, error)) {
            return false;
        }
    }
    return true;
}

bool emit_dynamic_sources(CppWriter& writer, const ir::Component& component,
                          const DynamicPlan& plan, const std::vector<EventAdapter>& adapters,
                          const std::vector<DynamicText>& dynamic_text,
                          const std::vector<DynamicMetadata>& dynamic_metadata,
                          const std::vector<ControlledInput>& controlled_inputs,
                          std::string& error) {
    for (const DynamicGroup& group : plan.groups) {
        if (!emit_apply_children(writer, component, plan, group, error)) {
            return false;
        }
    }
    return emit_list_sources(writer, component, plan, adapters, dynamic_text, dynamic_metadata,
                             controlled_inputs, error);
}

} // namespace tessera::compiler::codegen::internal
