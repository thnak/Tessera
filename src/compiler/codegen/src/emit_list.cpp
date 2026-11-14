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
#include "emit_props.hpp"
#include "emit_virtual.hpp"
#include "value_ops.hpp"

#include <cstdint>
#include <optional>
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

// The key of a list item as a numeric automation key, when its C++ form is an integer: the loop
// index, an `int` item, or an integer literal. A string key is matched by the generated key table
// instead (automation-ids-v1 section 1: `(UiId, ItemKey)`).
std::optional<std::string> numeric_item_key(const ListTemplate& list,
                                            const ir::Component& component,
                                            const LocalBindings& locals) {
    const ir::Expr& key = *list.node->key;
    bool integral = false;
    if (key.kind == ir::ExprKind::Literal) {
        integral = std::holds_alternative<std::int64_t>(key.literal);
    } else if (key.kind == ir::ExprKind::Ref) {
        if (const LocalBinding* local = find_local(locals, key.id)) {
            integral = local->ir_type == "int";
        }
    }
    if (!integral) {
        return std::nullopt;
    }
    const ExprEmission emission = emit_expr(key, component, locals);
    if (!emission.error.empty()) {
        return std::nullopt;
    }
    return emission.text;
}

bool emit_build_item(CppWriter& writer, const ir::Component& component, const ir::IrModule& module,
                     const DynamicPlan& plan, const ListTemplate& list,
                     const std::vector<EventAdapter>& adapters,
                     const std::vector<DynamicText>& dynamic_text,
                     const std::vector<DynamicMetadata>& dynamic_metadata,
                     const std::vector<ControlledInput>& controlled_inputs,
                     const std::vector<ComponentInstance>& instances, std::string& error) {
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
    state.module = &module;
    state.instances = &instances;
    // The item record owns the template's conditional branch slots, dynamic group parents and
    // per-item component instances (docs/design/compiler-node-state.md sections 3 and 4).
    state.item_out = "tessera_out";
    state.next_id = list.ordinal;
    state.next_var = list.ordinal;
    std::string root;
    if (!emit_node(writer, *list.node, state, root, error)) {
        return false;
    }
    // The item's automation identity is `(template uiId, item key)` (automation-ids-v1 section 1).
    // The template nodes already carry the template's automation id; a numeric key is also
    // published on the item root so the automation layer can address exactly one live item.
    if (const std::optional<std::string> key = numeric_item_key(list, component, locals)) {
        writer.line(root + "->set_item_key(static_cast<std::uint64_t>(" + *key + "));");
    }
    // No std::move: the returned unique_ptr is a local, and GCC's -Wpessimizing-move (CI builds
    // with -Werror) rejects a move that prevents copy elision.
    writer.line("return " + root + ";");
    writer.close("");
    writer.line("");
    return true;
}

bool emit_apply_item(CppWriter& writer, const ir::Component& component, const DynamicPlan& plan,
                     const ListTemplate& list, const std::vector<ComponentInstance>& instances,
                     std::string& error) {
    const std::string ordinal = std::to_string(list.ordinal);
    const std::string table = item_table_prefix(list);
    writer.open("void " + component.name + "::apply_item_" + ordinal +
                "(std::size_t tessera_slot, const " + list.element_cpp + "& " + list.item +
                ", std::size_t tessera_index)");
    writer.line("(void)" + list.item + ";");
    writer.line("(void)tessera_index;");
    const LocalBindings locals = list_locals(list, "static_cast<int>(tessera_index)");
    // Per-item child sets next: a branch attaches/detaches inside the item, at its source position.
    for (const DynamicGroup& group : plan.groups) {
        if (group.template_ordinal != list.ordinal) {
            continue;
        }
        writer.line("apply_item_children_" + std::to_string(group.parent_ordinal) + "(" + table +
                    "_items_[tessera_slot], " + list.item + ", tessera_index);");
    }
    // Per-item props of this item's component instances: hash-guarded, so an unchanged item copies
    // nothing (docs/design/compiler-node-state.md section 3).
    for (const ComponentInstance& instance : instances) {
        if (instance.template_ordinal != list.ordinal) {
            continue;
        }
        const std::string field =
            table + "_items_[tessera_slot].child_" + std::to_string(instance.ordinal);
        writer.line("// `" + instance.component_id + "` instance props of this item.");
        if (!emit_item_instance_props(writer, component, instance, table + "_items_[tessera_slot]",
                                      field, locals, false, error)) {
            return false;
        }
    }
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
    // A key that reads only the loop index leaves the item binding unused in this pass; marking it
    // used keeps the generated file warning-clean without changing the emitted code.
    writer.line("(void)" + list.item + ";");
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
    writer.line(
        "// Take over the previous table: its items, hashes and keys become the reuse pool.");
    writer.line("const std::size_t tessera_old_count = list_" + ordinal + "_count_;");
    writer.line(
        "std::vector<std::unique_ptr<tessera::ui::Node>> tessera_old_nodes(tessera_old_count);");
    writer.line("std::vector<Item_" + ordinal + "> tessera_old_items = std::move(list_" + ordinal +
                "_items_);");
    writer.line("std::vector<std::uint64_t> tessera_old_hashes = std::move(list_" + ordinal +
                "_hashes_);");
    writer.line("std::vector<bool> tessera_old_set = std::move(list_" + ordinal + "_set_);");
    writer.line("std::vector<std::string> tessera_old_keys = std::move(list_" + ordinal +
                "_keys_);");
    writer.open("for (std::size_t tessera_slot = 0; tessera_slot < tessera_old_count; "
                "++tessera_slot)");
    writer.line("tessera_old_nodes[tessera_slot] = "
                "parent.remove_child(*tessera_old_items[tessera_slot].root);");
    writer.close("");
    writer.line("// The new table is sized to the source: there is no item cap.");
    writer.line("std::vector<bool> tessera_reused(tessera_old_count);");
    writer.line("std::vector<std::string> tessera_next_keys(tessera_position);");
    writer.line("std::vector<Item_" + ordinal + "> tessera_next_items(tessera_position);");
    writer.line("std::vector<std::uint64_t> tessera_next_hashes(tessera_position);");
    writer.line("std::vector<bool> tessera_next_set(tessera_position);");
    writer.line("std::size_t tessera_next = 0;");
    writer.line("// Id slots held by any item of the previous table; a new item takes a free one.");
    writer.line("std::vector<bool> tessera_id_used(tessera_old_count + tessera_position + 1);");
    writer.open("for (std::size_t tessera_slot = 0; tessera_slot < tessera_old_count; "
                "++tessera_slot)");
    writer.line("tessera_id_used[tessera_old_items[tessera_slot].id_slot] = true;");
    writer.close("");
    writer.open("for (const " + list.element_cpp + "& " + list.item + " : " + source.text + ")");
    const ExprEmission key = emit_expr(*list.node->key, component, next_locals);
    if (!key.error.empty()) {
        return fail(error, "the key of `" + list.node->id + "`: " + key.error);
    }
    writer.line("std::string tessera_key = format_value(" + key.text + ");");
    writer.line("std::size_t tessera_match = tessera_old_count;");
    writer.open("for (std::size_t tessera_slot = 0; tessera_slot < tessera_old_count; "
                "++tessera_slot)");
    writer.open("if (!tessera_reused[tessera_slot] && tessera_old_keys[tessera_slot] == "
                "tessera_key)");
    writer.line("tessera_reused[tessera_slot] = true;");
    writer.line("tessera_match = tessera_slot;");
    writer.line("break;");
    writer.close("");
    writer.close("");
    writer.open("if (tessera_match != tessera_old_count)");
    writer.line("// Reuse: the node subtree, its interpolated text pointers, its per-item branch "
                "slots and its component instances move to the new slot, so the instance state "
                "follows the key across a reorder.");
    writer.line("tessera_next_items[tessera_next] = std::move(tessera_old_items[tessera_match]);");
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
    // A removed key drops its record: unmount its component instances (cleanups) and leave the
    // owner's scheduler before the subtree dies (docs/design/compiler-node-state.md section 3).
    if (!list.instance_ordinals.empty()) {
        writer.open("for (std::size_t tessera_slot = 0; tessera_slot < tessera_old_count; "
                    "++tessera_slot)");
        writer.open("if (tessera_reused[tessera_slot])");
        writer.line("continue;");
        writer.close("");
        for (const unsigned instance_ordinal : list.instance_ordinals) {
            const std::string field =
                "tessera_old_items[tessera_slot].child_" + std::to_string(instance_ordinal);
            writer.open("if (" + field + " != nullptr)");
            writer.line("if (mounted_) { " + field + "->unmount(); }");
            writer.line("if (scheduler_ != nullptr) { " + field +
                        "->detach_scheduler(*scheduler_); "
                        "}");
            writer.close("");
        }
        writer.close("");
    }
    writer.line("// Publish the new table; vector sizes track the live count, so no stale slot "
                "can match a removed key.");
    writer.line("list_" + ordinal + "_items_ = std::move(tessera_next_items);");
    writer.line("list_" + ordinal + "_hashes_ = std::move(tessera_next_hashes);");
    writer.line("list_" + ordinal + "_set_ = std::move(tessera_next_set);");
    writer.line("list_" + ordinal + "_keys_ = std::move(tessera_next_keys);");
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

bool emit_list_sources(CppWriter& writer, const ir::Component& component,
                       const ir::IrModule& module, const DynamicPlan& plan,
                       const std::vector<EventAdapter>& adapters,
                       const std::vector<DynamicText>& dynamic_text,
                       const std::vector<DynamicMetadata>& dynamic_metadata,
                       const std::vector<ControlledInput>& controlled_inputs,
                       const std::vector<ComponentInstance>& instances, std::string& error) {
    for (const ListTemplate& list : plan.lists) {
        if (!emit_build_item(writer, component, module, plan, list, adapters, dynamic_text,
                             dynamic_metadata, controlled_inputs, instances, error)) {
            return false;
        }
        if (!emit_apply_item(writer, component, plan, list, instances, error)) {
            return false;
        }
        if (list.is_virtual) {
            if (!emit_apply_virtual(writer, component, list, error)) {
                return false;
            }
            emit_update_virtual_geometry(writer, component, list);
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
                          const ir::IrModule& module, const DynamicPlan& plan,
                          const std::vector<EventAdapter>& adapters,
                          const std::vector<DynamicText>& dynamic_text,
                          const std::vector<DynamicMetadata>& dynamic_metadata,
                          const std::vector<ControlledInput>& controlled_inputs,
                          const std::vector<ComponentInstance>& instances, std::string& error) {
    for (const DynamicGroup& group : plan.groups) {
        if (group.in_template()) {
            if (!emit_item_apply_children(writer, component, plan, group, error)) {
                return false;
            }
            continue;
        }
        if (!emit_apply_children(writer, component, plan, group, error)) {
            return false;
        }
    }
    return emit_list_sources(writer, component, module, plan, adapters, dynamic_text,
                             dynamic_metadata, controlled_inputs, instances, error);
}

bool plan_has_item_instances(const DynamicPlan& plan) {
    for (const ListTemplate& list : plan.lists) {
        if (!list.instance_ordinals.empty()) {
            return true;
        }
    }
    return false;
}

void emit_item_instance_calls(CppWriter& writer, const DynamicPlan& plan, std::string_view method,
                              std::string_view argument, bool reverse) {
    for (const ListTemplate& list : plan.lists) {
        const std::string table = "list_" + std::to_string(list.ordinal) + "_items_";
        for (const unsigned ordinal : list.instance_ordinals) {
            const std::string child = "tessera_slot.child_" + std::to_string(ordinal);
            if (reverse) {
                writer.open("for (std::size_t tessera_item = " + table +
                            ".size(); tessera_item-- > 0;)");
            } else {
                writer.open("for (std::size_t tessera_item = 0; tessera_item < " + table +
                            ".size(); ++tessera_item)");
            }
            writer.line("auto& tessera_slot = " + table + "[tessera_item];");
            writer.line("if (" + child + " == nullptr) {");
            writer.indent += 1;
            writer.line("continue;");
            writer.indent -= 1;
            writer.line("}");
            if (argument.empty()) {
                writer.line(child + "->" + std::string(method) + "();");
            } else {
                writer.line(child + "->" + std::string(method) + "(" + std::string(argument) +
                            ");");
            }
            writer.close("");
        }
    }
}

} // namespace tessera::compiler::codegen::internal
