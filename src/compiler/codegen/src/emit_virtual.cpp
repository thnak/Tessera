// SPDX-License-Identifier: MIT

// Generated C++ for VirtualList/VirtualGrid item templates (ir-v1.md section 3.5,
// virtualization-v1.md section 10). The collection is an ordinary container node; the item
// template's realized window is attached to it by apply_virtual_<n>(), which reuses the keyed-list
// emission shape: a bounded item table, key-first change detection, keyed node reuse and a
// hash-compared per-item content pass. The visible-range math itself belongs to the runtime
// (`tessera::ui::virtualization::ItemGeometry`), so the generated method receives the half-open
// [first, last) range from the host and never re-implements prefix sums.

#include "emit_virtual.hpp"
#include "value_ops.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

bool is_grid(const ir::Node& collection) {
    return collection.kind == ir::NodeKind::VirtualGrid;
}

bool is_geometry_prop(std::string_view name, bool grid) {
    if (name == "estimateSize" || name == "overscan" || name == "gap" || name == "orientation") {
        return true;
    }
    if (name == "itemSize") {
        return !grid;
    }
    return grid && (name == "rowHeight" || name == "columns" || name == "columnWidth");
}

std::optional<double> number_literal(const ir::Expr* expr) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return std::nullopt;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&expr->literal)) {
        return static_cast<double>(*integer);
    }
    if (const auto* number = std::get_if<double>(&expr->literal)) {
        return *number;
    }
    return std::nullopt;
}

std::optional<std::int64_t> integer_literal(const ir::Expr* expr) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return std::nullopt;
    }
    if (const auto* integer = std::get_if<std::int64_t>(&expr->literal)) {
        return *integer;
    }
    if (const auto* number = std::get_if<double>(&expr->literal)) {
        return static_cast<std::int64_t>(*number);
    }
    return std::nullopt;
}

std::string double_text(double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof buffer, value);
    std::string out(buffer, result.ptr);
    if (out.find_first_of(".eE") == std::string::npos) {
        out += ".0";
    }
    return out;
}

const ir::Expr* prop(const ir::Node& node, std::string_view name) {
    const auto found = node.props.find(std::string(name));
    return found == node.props.end() ? nullptr : found->second.get();
}

// The item and index locals of one virtual template for the emitted C++ statements: the item is
// bound by `const T&` from the source list, the index is the absolute item index.
LocalBindings virtual_locals(const ListTemplate& collection, std::string_view index_cpp) {
    LocalBindings locals;
    locals.push_back(LocalBinding{collection.item, collection.item, collection.element_type});
    if (!collection.index.empty()) {
        locals.push_back(LocalBinding{collection.index, std::string(index_cpp), "int"});
    }
    return locals;
}

} // namespace

bool virtual_is_horizontal(const ir::Node& collection) {
    const auto found = collection.props.find("orientation");
    if (found == collection.props.end() || found->second == nullptr) {
        return false;
    }
    const auto* text = std::get_if<std::string>(&found->second->literal);
    return text != nullptr && *text == "horizontal";
}

std::optional<std::int64_t> virtual_integer_literal(const ir::Expr* expr) {
    return integer_literal(expr);
}

std::string item_table_prefix(const ListTemplate& list) {
    // A virtual table is named after its collection node (the host applies a window to it); an
    // eager list table is named after its template node. Members append their own `_` suffix
    // (`virtual_3_items_`, `list_5_keys_`).
    if (list.is_virtual) {
        return "virtual_" + std::to_string(list.container_ordinal);
    }
    return "list_" + std::to_string(list.ordinal);
}

bool validate_virtual_geometry(const ir::Node& collection, std::string& error) {
    const bool grid = is_grid(collection);
    const std::string tag = grid ? "VirtualGrid" : "VirtualList";
    for (const auto& [name, value] : collection.props) {
        if (!is_geometry_prop(name, grid)) {
            continue; // other props are emitted by the static/dynamic metadata paths
        }
        if (name == "orientation") {
            const ir::Expr* expr = value.get();
            if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
                return fail(error, "`<" + tag + ">` prop `orientation` must be a string literal");
            }
            const auto* text = std::get_if<std::string>(&expr->literal);
            if (text == nullptr || (*text != "vertical" && *text != "horizontal")) {
                return fail(error, "`<" + tag +
                                       ">` prop `orientation` must be \"vertical\" or "
                                       "\"horizontal\"");
            }
            continue;
        }
        if (name == "columns") {
            const std::optional<std::int64_t> columns = integer_literal(value.get());
            if (!columns || *columns < 1) {
                return fail(error,
                            "`<" + tag +
                                ">` prop `columns` must be an integer literal of at least 1");
            }
            continue;
        }
        if (!number_literal(value.get()).has_value()) {
            return fail(error, "`<" + tag + ">` prop `" + name +
                                   "` must be a number literal in this slice; dynamic virtual "
                                   "geometry has no runtime API yet");
        }
    }
    return true;
}

void emit_virtual_geometry_constants(CppWriter& writer, const ListTemplate& collection) {
    const ir::Node& node = *collection.container;
    const bool grid = is_grid(node);
    const std::string base = "virtual_" + std::to_string(collection.container_ordinal) + "_";
    const std::optional<double> item_size = number_literal(prop(node, "itemSize"));
    const std::optional<double> estimate = number_literal(prop(node, "estimateSize"));
    const std::optional<double> row_height = number_literal(prop(node, "rowHeight"));
    const std::optional<double> column_width = number_literal(prop(node, "columnWidth"));
    const std::optional<double> overscan = number_literal(prop(node, "overscan"));
    const std::optional<double> gap = number_literal(prop(node, "gap"));
    const std::optional<std::int64_t> columns = integer_literal(prop(node, "columns"));
    // virtualization-v1.md section 4 and the section 13 register decision: an unmeasured item
    // contributes a 32 px main-axis estimate; there is no per-item guess and no growth heuristic.
    double effective_estimate = 32.0;
    if (estimate) {
        effective_estimate = *estimate;
    } else if (grid && row_height) {
        effective_estimate = *row_height;
    } else if (!grid && item_size) {
        effective_estimate = *item_size;
    }
    writer.line("// `" + node.id + "` " + (grid ? "VirtualGrid" : "VirtualList") +
                " geometry (virtualization-v1.md section 10). The host feeds these into the");
    writer.line("// tessera_virtual geometry, which owns the prefix sums and the visible range.");
    writer.line("static constexpr double " + base + "item_size_ = " +
                double_text(item_size.value_or(0.0)) + "; // 0 means variable-size");
    writer.line("static constexpr double " + base +
                "estimate_size_ = " + double_text(effective_estimate) + ";");
    writer.line("static constexpr double " + base +
                "overscan_ = " + double_text(overscan.value_or(200.0)) + ";");
    writer.line("static constexpr double " + base + "gap_ = " + double_text(gap.value_or(0.0)) +
                ";");
    writer.line("static constexpr bool " + base +
                "horizontal_ = " + (virtual_is_horizontal(node) ? "true" : "false") +
                "; // main axis is x instead of y");
    if (grid) {
        writer.line("static constexpr double " + base + "row_height_ = " +
                    double_text(row_height.value_or(0.0)) + "; // 0 means variable row height");
        writer.line("static constexpr int " + base + "columns_ = " +
                    std::to_string(columns.value_or(0)) + "; // 0 means column-width mode");
        writer.line("static constexpr double " + base +
                    "column_width_ = " + double_text(column_width.value_or(0.0)) + ";");
    }
}

bool emit_apply_virtual(CppWriter& writer, const ir::Component& component,
                        const ListTemplate& collection, std::string& error) {
    const std::string ordinal = std::to_string(collection.ordinal);
    const std::string container_ordinal = std::to_string(collection.container_ordinal);
    const std::string table = item_table_prefix(collection);
    const ir::Loop& loop = *collection.node->loop;
    writer.open("void " + component.name + "::apply_virtual_" + container_ordinal +
                "(std::size_t tessera_first, std::size_t tessera_last)");
    writer.line("// The realized range is computed by the host with");
    writer.line("// tessera::ui::virtualization::ItemGeometry (virtualization-v1.md section 4):");
    writer.line("// the generated code applies the window and never re-implements prefix sums.");
    writer.open("if (" + table + "_ == nullptr)");
    writer.line("return;");
    writer.close("");
    const ExprEmission source = emit_expr(*loop.source, component);
    if (!source.error.empty()) {
        return fail(error, "the source of `" + collection.node->id + "`: " + source.error);
    }
    writer.line("const auto& tessera_source = " + source.text + ";");
    writer.line("const std::size_t tessera_total = tessera_source.size();");
    writer.open("if (tessera_first > tessera_total)");
    writer.line("tessera_first = tessera_total;");
    writer.close("");
    writer.open("if (tessera_last > tessera_total)");
    writer.line("tessera_last = tessera_total;");
    writer.close("");
    writer.open("if (tessera_last < tessera_first)");
    writer.line("tessera_last = tessera_first;");
    writer.close("");
    const LocalBindings key_locals = virtual_locals(collection, "static_cast<int>(tessera_i)");
    writer.line("// Key/order hash first: an unchanged window allocates nothing and touches no "
                "node.");
    writer.line("std::uint64_t tessera_order = tessera::ui::kEffectHashSeed;");
    writer.line("std::size_t tessera_window = 0;");
    writer.open("for (std::size_t tessera_i = tessera_first; tessera_i < tessera_last; "
                "++tessera_i)");
    writer.line("const " + collection.element_cpp + "& " + collection.item +
                " = tessera_source[tessera_i];");
    if (!emit_value_ops(writer, *collection.node->key, component, key_locals, "tessera_order", "",
                        error)) {
        return false;
    }
    writer.line("++tessera_window;");
    writer.close("");
    writer.line("tessera_order = tessera::ui::effect_hash(tessera_order, "
                "static_cast<int>(tessera_first));");
    writer.line("tessera_order = tessera::ui::effect_hash(tessera_order, "
                "static_cast<int>(tessera_window));");
    writer.open("if (!" + table + "_order_set_ || tessera_order != " + table + "_order_hash_)");
    writer.line(table + "_order_set_ = true;");
    writer.line(table + "_order_hash_ = tessera_order;");
    writer.line(
        "// Take over the current window: its items, hashes and keys become the reuse pool.");
    writer.line("const std::size_t tessera_old_count = " + table + "_count_;");
    writer.line("std::vector<std::unique_ptr<tessera::ui::Node>> tessera_old_nodes("
                "tessera_old_count);");
    writer.line("std::vector<Item_" + ordinal + "> tessera_old_items = std::move(" + table +
                "_items_);");
    writer.line("std::vector<std::uint64_t> tessera_old_hashes = std::move(" + table +
                "_hashes_);");
    writer.line("std::vector<bool> tessera_old_set = std::move(" + table + "_set_);");
    writer.line("std::vector<std::string> tessera_old_keys = std::move(" + table + "_keys_);");
    writer.open("for (std::size_t tessera_slot = 0; tessera_slot < tessera_old_count; "
                "++tessera_slot)");
    writer.line("tessera_old_nodes[tessera_slot] = " + table +
                "_->remove_child(*tessera_old_items[tessera_slot].root);");
    writer.close("");
    writer.line("// The new window is sized to the realized range: there is no item cap.");
    writer.line("std::vector<bool> tessera_reused(tessera_old_count);");
    writer.line("std::vector<std::string> tessera_next_keys(tessera_window);");
    writer.line("std::vector<Item_" + ordinal + "> tessera_next_items(tessera_window);");
    writer.line("std::vector<std::uint64_t> tessera_next_hashes(tessera_window);");
    writer.line("std::vector<bool> tessera_next_set(tessera_window);");
    writer.line("std::size_t tessera_next = 0;");
    writer.open("for (std::size_t tessera_i = tessera_first; tessera_i < tessera_last; "
                "++tessera_i)");
    writer.line("const " + collection.element_cpp + "& " + collection.item +
                " = tessera_source[tessera_i];");
    const ExprEmission key = emit_expr(*collection.node->key, component, key_locals);
    if (!key.error.empty()) {
        return fail(error, "the key of `" + collection.node->id + "`: " + key.error);
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
    writer.line("// Reuse: the subtree, its text pointers and its content hash move to the new "
                "slot.");
    writer.line("tessera_next_items[tessera_next] = tessera_old_items[tessera_match];");
    writer.line("tessera_next_hashes[tessera_next] = tessera_old_hashes[tessera_match];");
    writer.line("tessera_next_set[tessera_next] = tessera_old_set[tessera_match];");
    writer.line(table + "_->insert_child(tessera_next, "
                        "std::move(tessera_old_nodes[tessera_match]));");
    writer.close("");
    writer.line("else {");
    ++writer.indent;
    writer.line("// The absolute item index is the NodeId slot: an item keeps its id across "
                "scrolls and no two live windows can collide.");
    writer.line("std::unique_ptr<tessera::ui::Node> tessera_built = build_item_" + ordinal + "(" +
                collection.item + ", tessera_i, tessera_i, tessera_next_items[tessera_next]);");
    writer.line("tessera_next_items[tessera_next].root = tessera_built.get();");
    writer.line("tessera_next_hashes[tessera_next] = 0;");
    writer.line("tessera_next_set[tessera_next] = false;");
    writer.line(table + "_->insert_child(tessera_next, std::move(tessera_built));");
    --writer.indent;
    writer.line("}");
    writer.line("tessera_next_keys[tessera_next] = std::move(tessera_key);");
    writer.line("++tessera_next;");
    writer.close("");
    writer.line("// Publish the new window; vector sizes track the live count, so no stale slot "
                "can match a removed key.");
    writer.line(table + "_items_ = std::move(tessera_next_items);");
    writer.line(table + "_hashes_ = std::move(tessera_next_hashes);");
    writer.line(table + "_set_ = std::move(tessera_next_set);");
    writer.line(table + "_keys_ = std::move(tessera_next_keys);");
    writer.line(table + "_count_ = tessera_next;");
    writer.line(table + "_first_ = tessera_first;");
    writer.close("");
    writer.line("// Content pass: always runs, hash-compared per item, so an unchanged item marks "
                "nothing dirty.");
    writer.line("std::size_t tessera_slot = 0;");
    writer.open("for (std::size_t tessera_i = tessera_first; tessera_i < tessera_last; "
                "++tessera_i)");
    writer.open("if (tessera_slot == " + table + "_count_)");
    writer.line("break;");
    writer.close("");
    writer.line("apply_item_" + ordinal + "(tessera_slot, tessera_source[tessera_i], tessera_i);");
    writer.line("++tessera_slot;");
    writer.close("");
    writer.close("");
    writer.line("");
    return true;
}

} // namespace tessera::compiler::codegen::internal
