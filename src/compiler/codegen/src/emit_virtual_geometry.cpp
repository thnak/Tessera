// SPDX-License-Identifier: MIT

// Geometry emission for VirtualList/VirtualGrid (ir-v1.md section 3.5, virtualization-v1.md
// sections 4, 6 and 10; docs/design/compiler.md section 11).
//
// A collection's numeric geometry inputs (`estimateSize`, `gap`, `itemSize`, `rowHeight`) may come
// from a state/prop cell instead of a literal. The generated `update_virtual_geometry_<n>()` is the
// re-seed path: it re-evaluates the inputs, hash-compares them against the last seed (so a
// per-frame call is one FNV-1a pass and never rebuilds), keeps the measurements the change cannot
// invalidate, and re-anchors the caller's offset so the item at the viewport start keeps its
// viewport position.
//
// The structural inputs (`orientation`, `columns`, `overscan`, `columnWidth`) accept a literal or a
// component prop reference. A literal is still baked into a `static constexpr` (byte-identical
// output); a prop-driven one is a mutable member refreshed by `scroll_virtual_<n>()` at sync time,
// so a prop change re-realizes the window on the next sync instead of requiring a codegen change.

#include "emit_virtual.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
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

std::optional<std::string> string_literal(const ir::Expr* expr) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return std::nullopt;
    }
    if (const auto* text = std::get_if<std::string>(&expr->literal)) {
        return *text;
    }
    return std::nullopt;
}

// The declared IR type of a `state::`/`prop::`/`anim::` reference, or nullopt when the id names no
// declared cell (IR validation reports that separately; codegen refuses to guess a type).
std::optional<std::string> declared_type(const ir::Component& component, std::string_view id) {
    const std::size_t split = id.find("::");
    if (split == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view prefix = id.substr(0, split);
    const std::string_view name = id.substr(split + 2);
    if (prefix == "prop") {
        for (const ir::Prop& declared : component.props) {
            if (declared.name == name) {
                return declared.type;
            }
        }
    } else if (prefix == "state") {
        for (const ir::State& cell : component.state) {
            if (cell.id == id) {
                return cell.type;
            }
        }
    } else if (prefix == "anim") {
        for (const ir::AnimatedValue& cell : component.animated_values) {
            if (cell.id == id) {
                return cell.type;
            }
        }
    }
    return std::nullopt;
}

bool numeric_type(std::string_view type) {
    return type == "int" || type == "float" || type == "length";
}

bool string_type(std::string_view type) {
    return type == "string";
}

bool int_type(std::string_view type) {
    return type == "int";
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

// Resolves a structural option that is not a literal. Only a `prop::` reference is runtime-driven
// in this slice, and its declared type must satisfy the option's contract; `shape` names the
// literal form so one typed message covers a computed expression and a wrong reference kind.
bool resolve_prop_option(const ir::Component& component, const std::string& context,
                         std::string_view shape, const ir::Expr& expr,
                         bool (*type_ok)(std::string_view), std::string_view expected_type,
                         VirtualOption& out, std::string& error) {
    if (expr.kind != ir::ExprKind::Ref) {
        return fail(error, context + " must be " + std::string(shape) +
                               " or a `props.<name>` component prop; a computed expression is a "
                               "later slice");
    }
    if (!starts_with(expr.id, "prop::")) {
        return fail(error, context + " must be " + std::string(shape) +
                               " or a `props.<name>` component prop; `" + expr.id +
                               "` is neither (a state-driven structural option is a later slice)");
    }
    const std::optional<std::string> type = declared_type(component, expr.id);
    if (!type) {
        return fail(error, context + " references `" + expr.id + "`, which is not a declared prop");
    }
    if (!type_ok(*type)) {
        return fail(error, context + " references `" + expr.id + "` of type `" + *type +
                               "`; it needs a `" + std::string(expected_type) + "` prop");
    }
    const ExprEmission emitted = emit_expr(expr, component);
    if (!emitted.error.empty()) {
        return fail(error, context + ": " + emitted.error);
    }
    out.prop_driven = true;
    out.cpp = emitted.text;
    return true;
}

// Resolves one numeric geometry input: a number literal, or a numeric state/prop reference the
// re-seed path re-evaluates. Anything else is a loud rejection (no implicit string-to-double).
bool resolve_value(const ir::Component& component, const std::string& tag, std::string_view name,
                   const ir::Expr* expr, VirtualGeometryValue& out, std::string& error) {
    out = VirtualGeometryValue{};
    if (expr == nullptr) {
        return true;
    }
    out.present = true;
    const std::string context = "`<" + tag + ">` prop `" + std::string(name) + "`";
    if (const std::optional<double> number = number_literal(expr)) {
        out.literal = true;
        out.literal_value = *number;
        out.cpp = double_text(*number);
        return true;
    }
    if (expr->kind == ir::ExprKind::Literal) {
        return fail(error, context + " must be a number literal");
    }
    if (expr->kind != ir::ExprKind::Ref) {
        return fail(error, context +
                               " must be a number literal or a numeric state/prop reference; a "
                               "computed geometry expression is a later slice");
    }
    const std::optional<std::string> type = declared_type(component, expr->id);
    if (!type) {
        return fail(error, context + " references `" + expr->id +
                               "`, which is not a declared "
                               "state, prop or animated value");
    }
    if (!numeric_type(*type)) {
        return fail(error, context + " references `" + expr->id + "` of type `" + *type +
                               "`; a geometry value must be numeric (int, float or length)");
    }
    const ExprEmission emitted = emit_expr(*expr, component);
    if (!emitted.error.empty()) {
        return fail(error, context + ": " + emitted.error);
    }
    out.cpp = "static_cast<double>(" + emitted.text + ")";
    return true;
}

} // namespace

bool virtual_geometry_spec(const ir::Component& component, const ir::Node& collection,
                           VirtualGeometrySpec& out, std::string& error) {
    out = VirtualGeometrySpec{};
    out.grid = collection.kind == ir::NodeKind::VirtualGrid;
    const std::string tag = out.grid ? "VirtualGrid" : "VirtualList";

    // Structural options, checked in name order for a deterministic first error. A literal keeps
    // the emitted compile-time constant; a `props.<name>` reference becomes a sync-time member.
    if (const ir::Expr* orientation = prop(collection, "orientation")) {
        const std::string context = "`<" + tag + ">` prop `orientation`";
        if (const std::optional<std::string> text = string_literal(orientation)) {
            if (*text != "vertical" && *text != "horizontal") {
                return fail(error, context + " must be \"vertical\" or \"horizontal\"");
            }
            out.horizontal = *text == "horizontal";
        } else if (!resolve_prop_option(component, context, "\"vertical\" or \"horizontal\"",
                                        *orientation, string_type, "string", out.orientation_source,
                                        error)) {
            return false;
        }
    }
    if (out.grid) {
        if (const ir::Expr* columns = prop(collection, "columns")) {
            const std::string context = "`<" + tag + ">` prop `columns`";
            if (const std::optional<std::int64_t> count = virtual_integer_literal(columns)) {
                if (*count < 1) {
                    return fail(error, context + " must be an integer literal of at least 1");
                }
                out.columns = *count;
            } else if (!resolve_prop_option(component, context, "an integer literal of at least 1",
                                            *columns, int_type, "int", out.columns_source, error)) {
                return false;
            }
        }
        if (const ir::Expr* width = prop(collection, "columnWidth")) {
            const std::string context = "`<" + tag + ">` prop `columnWidth`";
            if (const std::optional<double> value = number_literal(width)) {
                out.column_width = *value;
            } else if (!resolve_prop_option(component, context, "a number literal", *width,
                                            numeric_type, "numeric", out.column_width_source,
                                            error)) {
                return false;
            }
        }
    }
    if (const ir::Expr* overscan = prop(collection, "overscan")) {
        const std::string context = "`<" + tag + ">` prop `overscan`";
        if (const std::optional<double> value = number_literal(overscan)) {
            out.overscan = *value;
        } else if (!resolve_prop_option(component, context, "a number literal", *overscan,
                                        numeric_type, "numeric", out.overscan_source, error)) {
            return false;
        }
    }

    // Size inputs: a list declares `itemSize`, a grid `rowHeight`.
    const std::string_view fixed_name = out.grid ? "rowHeight" : "itemSize";
    if (!resolve_value(component, tag, fixed_name, prop(collection, fixed_name), out.fixed_extent,
                       error)) {
        return false;
    }
    VirtualGeometryValue estimate;
    if (!resolve_value(component, tag, "estimateSize", prop(collection, "estimateSize"), estimate,
                       error)) {
        return false;
    }
    if (!estimate.present) {
        if (out.fixed_extent.present) {
            // The declared fixed extent is the effective estimate, so changing it invalidates the
            // measurements (virtualization-v1.md section 10's documented rule).
            estimate = out.fixed_extent;
            out.fixed_seeds_estimate = true;
        } else {
            // Section 4: an unmeasured item contributes a 32 px estimate; no per-item guess.
            estimate.present = true;
            estimate.literal = true;
            estimate.literal_value = 32.0;
            estimate.cpp = "32.0";
        }
    }
    out.estimate = estimate;
    if (!resolve_value(component, tag, "gap", prop(collection, "gap"), out.gap, error)) {
        return false;
    }
    if (!out.gap.present) {
        out.gap.present = true;
        out.gap.literal = true;
        out.gap.cpp = "0.0";
    }
    return true;
}

void emit_virtual_geometry_fields(CppWriter& writer, const ListTemplate& collection) {
    const ir::Node& node = *collection.container;
    const VirtualGeometrySpec& spec = collection.geometry;
    const std::string base = "virtual_" + std::to_string(collection.container_ordinal) + "_";
    writer.line("// `" + node.id + "` " + (spec.grid ? "VirtualGrid" : "VirtualList") +
                " geometry (virtualization-v1.md section 10). These are the last seeded values:");
    writer.line("// update_virtual_geometry_" + std::to_string(collection.container_ordinal) +
                "() re-evaluates them from their props/state and re-seeds the geometry below.");
    const std::string fixed_name = spec.grid ? "row_height_" : "item_size_";
    writer.line("// Declared fixed extent; 0 means variable-size.");
    if (spec.fixed_extent.present) {
        writer.line("double " + base + fixed_name + " = " + spec.fixed_extent.cpp + ";");
    } else {
        writer.line("static constexpr double " + base + fixed_name + " = 0.0;");
    }
    writer.line("double " + base + "estimate_size_ = " + spec.estimate.cpp + ";");
    if (spec.overscan_source.prop_driven) {
        writer.line("double " + base + "overscan_ = " + double_text(spec.overscan) +
                    "; // prop-driven: scroll_virtual_<n>() refreshes it");
    } else {
        writer.line("static constexpr double " + base +
                    "overscan_ = " + double_text(spec.overscan) + ";");
    }
    writer.line("double " + base + "gap_ = " + spec.gap.cpp + ";");
    if (spec.orientation_source.prop_driven) {
        writer.line("bool " + base + "horizontal_ = " + (spec.horizontal ? "true" : "false") +
                    "; // prop-driven: scroll_virtual_<n>() refreshes it");
    } else {
        writer.line("static constexpr bool " + base + "horizontal_ = " +
                    (spec.horizontal ? "true" : "false") + "; // main axis is x instead of y");
    }
    if (spec.grid) {
        if (spec.columns_source.prop_driven) {
            writer.line("int " + base + "columns_ = " + std::to_string(spec.columns) +
                        "; // prop-driven: 0 means column-width mode");
        } else {
            writer.line("static constexpr int " + base + "columns_ = " +
                        std::to_string(spec.columns) + "; // 0 means column-width mode");
        }
        if (spec.column_width_source.prop_driven) {
            writer.line("double " + base + "column_width_ = " + double_text(spec.column_width) +
                        "; // prop-driven: scroll_virtual_<n>() refreshes it");
        } else {
            writer.line("static constexpr double " + base +
                        "column_width_ = " + double_text(spec.column_width) + ";");
        }
    }
}

void emit_virtual_option_refresh(CppWriter& writer, const ListTemplate& collection) {
    const VirtualGeometrySpec& spec = collection.geometry;
    const std::string base = "virtual_" + std::to_string(collection.container_ordinal) + "_";
    const bool any = spec.orientation_source.prop_driven || spec.columns_source.prop_driven ||
                     spec.overscan_source.prop_driven || spec.column_width_source.prop_driven;
    if (!any) {
        return;
    }
    writer.line("// Structural options may come from component props: read them once per sync (no");
    writer.line("// allocation) and clamp to the runtime contract before the row/window math.");
    if (spec.orientation_source.prop_driven) {
        writer.line(base + "horizontal_ = (" + spec.orientation_source.cpp +
                    " == \"horizontal\");");
        writer.line("// An unknown orientation string falls back to vertical (the default axis).");
    }
    if (spec.overscan_source.prop_driven) {
        writer.line(base + "overscan_ = static_cast<double>(" + spec.overscan_source.cpp + ");");
        writer.open("if (" + base + "overscan_ < 0.0)");
        writer.line(base + "overscan_ = 0.0; // a negative overscan means no overscan");
        writer.close("");
    }
    if (spec.columns_source.prop_driven) {
        writer.line(base + "columns_ = static_cast<int>(" + spec.columns_source.cpp + ");");
        writer.open("if (" + base + "columns_ < 0)");
        writer.line(base + "columns_ = 0; // non-positive means column-width mode");
        writer.close("");
    }
    if (spec.column_width_source.prop_driven) {
        writer.line(base + "column_width_ = static_cast<double>(" + spec.column_width_source.cpp +
                    ");");
    }
}

void emit_update_virtual_geometry(CppWriter& writer, const ir::Component& component,
                                  const ListTemplate& collection) {
    const VirtualGeometrySpec& spec = collection.geometry;
    const std::string table = item_table_prefix(collection);
    const std::string ordinal = std::to_string(collection.container_ordinal);
    const std::string fixed_name = spec.grid ? "row_height_" : "item_size_";
    const bool drop_measures = spec.fixed_extent.present && spec.fixed_seeds_estimate;
    writer.open("bool " + component.name + "::update_virtual_geometry_" + ordinal +
                "(float& tessera_scroll, float tessera_viewport)");
    writer.line("// Geometry re-seed (virtualization-v1.md section 10): the props/state cells are");
    writer.line("// re-evaluated and hash-compared first, so a per-frame call costs one FNV-1a");
    writer.line("// pass and an unchanged geometry is never rebuilt.");
    if (spec.fixed_extent.present) {
        writer.line("const double tessera_fixed = " + spec.fixed_extent.cpp + ";");
    }
    writer.line("const double tessera_estimate = " + spec.estimate.cpp + ";");
    writer.line("const double tessera_gap = " + spec.gap.cpp + ";");
    writer.line("std::uint64_t tessera_hash = tessera::ui::kEffectHashSeed;");
    if (spec.fixed_extent.present) {
        writer.line("tessera_hash = tessera::ui::effect_hash(tessera_hash, tessera_fixed);");
    }
    writer.line("tessera_hash = tessera::ui::effect_hash(tessera_hash, tessera_estimate);");
    writer.line("tessera_hash = tessera::ui::effect_hash(tessera_hash, tessera_gap);");
    writer.open("if (" + table + "_geometry_set_ && tessera_hash == " + table + "_geometry_hash_)");
    writer.line("return false;");
    writer.close("");
    if (drop_measures) {
        writer.line("// A declared fixed extent describes every item, so changing it invalidates");
        writer.line("// the recorded measurements; an estimate or gap change never does (an");
        writer.line("// estimate only describes unmeasured items and a gap moves offsets, not");
        writer.line("// sizes).");
        writer.line("const bool tessera_drop_measures =");
        writer.line("    " + table + "_geometry_set_ && tessera_fixed != " + table + "_" +
                    fixed_name + ";");
    }
    writer.line(
        "// The viewport-start item keeps its viewport position across the change: capture");
    writer.line("// its prefix offset before the re-seed and shift by the delta afterwards.");
    writer.line("const std::size_t tessera_anchor =");
    writer.line("    " + table + "_geometry_.index_at_offset(tessera_scroll);");
    writer.line("const float tessera_anchor_offset =");
    writer.line("    " + table + "_geometry_.item_offset(tessera_anchor);");
    if (spec.fixed_extent.present) {
        writer.line(table + "_" + fixed_name + " = tessera_fixed;");
    }
    writer.line(table + "_estimate_size_ = tessera_estimate;");
    writer.line(table + "_gap_ = tessera_gap;");
    writer.line("// The hash guard above already proved the change is real; `reseed`'s own return");
    writer.line("// value only reports that, so it is intentionally discarded.");
    writer.line("(void)" + table + "_geometry_.reseed(static_cast<float>(tessera_estimate),");
    writer.line("    static_cast<float>(tessera_gap));");
    if (drop_measures) {
        writer.open("if (tessera_drop_measures)");
        writer.line(table + "_geometry_.clear_measurements();");
        writer.close("");
    }
    writer.line(table + "_geometry_hash_ = tessera_hash;");
    writer.line(table + "_geometry_set_ = true;");
    writer.line("++" + table + "_geometry_seeds_;");
    writer.line(
        "// Section 6 re-anchor with the old/new prefix sums: `refined_index` 0 means \"the");
    writer.line(
        "// whole content before the anchor\", so the shift is the aggregate delta and the");
    writer.line("// runtime rule clamps the result to [0, total - viewport].");
    writer.line("const tessera::ui::virtualization::AnchorAdjustment tessera_adjust =");
    writer.line("    tessera::ui::virtualization::reanchor_scroll(");
    writer.line("        tessera_scroll, tessera_viewport, " + table +
                "_geometry_.total_extent(),");
    writer.line("        tessera_anchor, 0,");
    writer.line("        " + table + "_geometry_.item_offset(tessera_anchor) - " +
                "tessera_anchor_offset);");
    writer.line("tessera_scroll = tessera_adjust.scroll_offset;");
    writer.line("return true;");
    writer.close("");
    writer.line("");
}

} // namespace tessera::compiler::codegen::internal
