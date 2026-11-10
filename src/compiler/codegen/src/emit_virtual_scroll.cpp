// SPDX-License-Identifier: MIT

// Collection-level scroll wiring for VirtualList/VirtualGrid (ir-v1.md section 3.5,
// virtualization-v1.md sections 4, 6 and 10; docs/design/compiler.md section 11).
//
// `scroll_virtual_<n>(scroll_offset, viewport_extent)` is the generated scroll entry point. It owns
// the collection's `tessera::ui::virtualization::ItemGeometry` (seeded from the emitted constants),
// feeds it the main-axis size a realized item reports through its laid-out scene bounds, re-anchors
// the offset with the runtime's section 6 rule, applies the realized window through
// `apply_virtual_<n>`, and then dispatches the collection-level handlers:
//
// - `onScroll` receives the scene `ScrollEventInfo` payload when its handler takes one (mapped to
//   absolute offsets and viewport/content extents), or a default `Event` when it takes no
//   parameter;
// - `onEndReached` (no payload in the schema) fires once per end-zone entry and is distance
//   throttled (see the emitted `kVirtualEndReachedThrottlePx` contract).
//
// The scene has no scroll `EventType` yet, so the driver is a host entry point on the component,
// not a scene event binding (the host owns the scroll offset and calls it). It never re-implements
// prefix sums and never allocates after the first application.

#include "emit_events.hpp"
#include "emit_virtual.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

const ir::Handler* find_handler(const ir::Component& component, std::string_view id) {
    for (const ir::Handler& handler : component.handlers) {
        if (handler.id == id) {
            return &handler;
        }
    }
    return nullptr;
}

const ir::Binding* find_binding(const std::vector<const ir::Binding*>& bindings,
                                std::string_view id) {
    for (const ir::Binding* binding : bindings) {
        if (binding->id == id) {
            return binding;
        }
    }
    return nullptr;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

// Resolves one collection event to a generated member function, validating its payload contract:
// `payload_record` is the one payload record the event accepts, or empty when the event takes no
// payload and a parameter is an error.
bool resolve_virtual_event(const ir::Component& component,
                           const std::vector<const ir::Binding*>& bindings,
                           const ir::Node& collection, std::string_view jsx_name,
                           std::string_view payload_record, VirtualEventTarget& out,
                           std::string& error) {
    const auto found = collection.events.find(std::string(jsx_name));
    if (found == collection.events.end()) {
        return true;
    }
    const std::string context = "`<" + std::string(ir::node_kind_name(collection.kind)) + ">` (`" +
                                collection.id + "`) event `" + std::string(jsx_name) + "`";
    const std::string& target = found->second;
    const std::vector<ir::Param>* params = nullptr;
    if (starts_with(target, "handler::")) {
        const ir::Handler* handler = find_handler(component, target);
        if (handler == nullptr) {
            return fail(error, context + " names unknown handler `" + target + "`");
        }
        params = &handler->params;
        out.member = method_name(handler->id);
    } else if (starts_with(target, "bind::")) {
        const ir::Binding* binding = find_binding(bindings, target);
        if (binding == nullptr) {
            return fail(error, context + " names unknown binding `" + target + "`");
        }
        if (binding->kind != ir::BindingKind::Event) {
            return fail(error,
                        context + " names `" + binding->id + "`, which is not an event binding");
        }
        if (!binding->return_type.empty()) {
            return fail(error, context + " binding `" + binding->id +
                                   "` must return void; a collection event has no return value");
        }
        params = &binding->params;
        out.member = binding->region;
    } else {
        return fail(error, context + " names neither a handler nor a binding");
    }
    out.present = true;
    if (params->empty()) {
        out.payload = false;
        return true;
    }
    if (payload_record.empty()) {
        return fail(error, context + " takes no payload; remove the handler parameter");
    }
    if (params->size() > 1) {
        return fail(error, context + " takes at most one parameter");
    }
    if (params->front().type != payload_record) {
        return fail(error, context + " payload `" + params->front().type + "` is not `" +
                               std::string(payload_record) + "`");
    }
    out.payload = true;
    return true;
}

// The `onScroll` call: the scene `ScrollEventInfo` record mapped to absolute offsets, or a default
// `Event` for a zero-parameter handler.
void emit_on_scroll_call(CppWriter& writer, const ListTemplate& collection,
                         const std::string& table, bool horizontal) {
    const VirtualEventTarget& target = collection.events.on_scroll;
    if (target.payload) {
        writer.line("tessera::ui::ScrollEventInfo tessera_scroll_info{};");
        writer.line(std::string("tessera_scroll_info.offset_") +
                    (horizontal ? "x = tessera_scroll;" : "y = tessera_scroll;"));
        writer.line(std::string("tessera_scroll_info.viewport_") +
                    (horizontal ? "width = tessera_viewport;" : "height = tessera_viewport;"));
        writer.line(std::string("tessera_scroll_info.content_") + (horizontal
                                                                       ? "width = " + table +
                                                                             "_geometry_."
                                                                             "total_extent();"
                                                                       : "height = " + table +
                                                                             "_geometry_."
                                                                             "total_extent();"));
        writer.line("this->" + target.member + "(tessera_scroll_info);");
        return;
    }
    writer.line("tessera::ui::Event tessera_scroll_event{};");
    writer.line(std::string("tessera_scroll_event.scroll_") +
                (horizontal ? "x = tessera_scroll;" : "y = tessera_scroll;"));
    writer.line("this->" + target.member + "(tessera_scroll_event);");
}

// The edge-triggered, distance-throttled `onEndReached` call (virtualization-v1.md section 10).
void emit_on_end_reached_call(CppWriter& writer, const ListTemplate& collection,
                              const std::string& table, bool grid) {
    const std::string rows = grid ? "tessera_rows" : "tessera_total";
    writer.line("// `onEndReached`: one fire per end-zone entry; a re-entry within");
    writer.line("// kVirtualEndReachedThrottlePx of the last fire is suppressed and counted.");
    writer.line("const bool tessera_at_end = " + rows + " > 0 && tessera_range.last >= " + rows +
                ";");
    writer.open("if (!tessera_at_end)");
    writer.line(table + "_end_armed_ = true;");
    writer.close("");
    writer.open("else if (" + table + "_end_armed_)");
    writer.line(table + "_end_armed_ = false;");
    writer.line("float tessera_end_distance = tessera_scroll - " + table + "_end_fire_offset_;");
    writer.open("if (tessera_end_distance < 0.0f)");
    writer.line("tessera_end_distance = -tessera_end_distance;");
    writer.close("");
    writer.line("if (" + table + "_end_fires_ > 0 &&");
    writer.line("    tessera_end_distance < kVirtualEndReachedThrottlePx) {");
    ++writer.indent;
    writer.line("++" + table + "_end_throttled_;");
    --writer.indent;
    writer.line("}");
    writer.line("else {");
    ++writer.indent;
    writer.line("++" + table + "_end_fires_;");
    writer.line(table + "_end_fire_offset_ = tessera_scroll;");
    writer.line("tessera::ui::Event tessera_end_event{};");
    writer.line("this->" + collection.events.on_end_reached.member + "(tessera_end_event);");
    --writer.indent;
    writer.line("}");
    writer.close("");
}

} // namespace

bool collect_virtual_events(const ir::Component& component,
                            const std::vector<const ir::Binding*>& bindings, DynamicPlan& plan,
                            std::string& error) {
    for (ListTemplate& list : plan.lists) {
        if (!list.is_virtual || list.container == nullptr) {
            continue;
        }
        VirtualEventWiring wiring;
        if (!resolve_virtual_event(component, bindings, *list.container, "onScroll", "ScrollEvent",
                                   wiring.on_scroll, error)) {
            return false;
        }
        if (!resolve_virtual_event(component, bindings, *list.container, "onEndReached", "",
                                   wiring.on_end_reached, error)) {
            return false;
        }
        // The generated row mapping needs a fixed column count; a column-width grid can still be
        // driven by the host through apply_virtual_<n>, but not by the generated scroll entry
        // point.
        if (list.container->kind == ir::NodeKind::VirtualGrid &&
            (wiring.on_scroll.present || wiring.on_end_reached.present)) {
            const auto columns = list.container->props.find("columns");
            if (columns == list.container->props.end() ||
                !virtual_integer_literal(columns->second.get()).has_value()) {
                return fail(error, "`<VirtualGrid>` (`" + list.container->id +
                                       "`) declares a collection event but no `columns`; the "
                                       "generated scroll handler maps rows with a fixed column "
                                       "count");
            }
        }
        list.events = std::move(wiring);
    }
    return true;
}

bool emit_virtual_scroll(CppWriter& writer, const ir::Component& component,
                         const ListTemplate& collection, std::string& error) {
    const ir::Node& node = *collection.container;
    const bool grid = node.kind == ir::NodeKind::VirtualGrid;
    const bool horizontal = virtual_is_horizontal(node);
    const std::string table = item_table_prefix(collection);
    const std::string ordinal = std::to_string(collection.container_ordinal);
    const std::string base = "virtual_" + ordinal + "_";
    const ir::Loop& loop = *collection.node->loop;
    const ExprEmission source = emit_expr(*loop.source, component);
    if (!source.error.empty()) {
        return fail(error, "the source of `" + node.id + "`: " + source.error);
    }
    writer.open("bool " + component.name + "::scroll_virtual_" + ordinal +
                "(float& tessera_scroll, float tessera_viewport)");
    writer.line("// Generated scroll entry point (virtualization-v1.md section 10): the component");
    writer.line("// owns the geometry, refines it from realized item measures, re-anchors the");
    writer.line("// offset, applies the realized window and dispatches the collection events.");
    writer.open("if (" + table + "_ == nullptr)");
    writer.line("return false;");
    writer.close("");
    writer.open("if (tessera_scroll < 0.0f)");
    writer.line("tessera_scroll = 0.0f;");
    writer.close("");
    writer.line("const auto& tessera_source = " + source.text + ";");
    writer.line("const std::size_t tessera_total = tessera_source.size();");
    writer.open("if (tessera_total > kMaxGeneratedVirtualGeometryItems)");
    writer.line("// Loud refusal: drive apply_virtual_" + ordinal +
                "() with a caller-owned geometry instead.");
    writer.line("return false;");
    writer.close("");
    if (grid) {
        writer.line("const std::size_t tessera_columns = static_cast<std::size_t>(" + base +
                    "columns_);");
        writer.open("if (tessera_columns == 0)");
        writer.line("// Column-width mode has no generated row mapping; the host drives the");
        writer.line("// window with its own tessera::ui::virtualization::VirtualGridRows.");
        writer.line("return false;");
        writer.close("");
        writer.line("const std::size_t tessera_rows =");
        writer.line("    (tessera_total + tessera_columns - 1) / tessera_columns;");
    } else {
        writer.line("const std::size_t tessera_rows = tessera_total;");
    }
    writer.open("if (tessera_rows != " + table + "_geometry_count_)");
    writer.open("if (!" + table + "_geometry_.set_count(tessera_rows))");
    writer.line("return false;");
    writer.close("");
    writer.line(table + "_geometry_count_ = tessera_rows;");
    writer.close("");
    writer.line("// Measurement feedback: a realized item reports its main-axis border-box extent");
    writer.line("// through its laid-out scene bounds; an unlaid-out (zero) item reports nothing.");
    writer.line(
        "// `tessera_anchor` is the item at the viewport start: a measure before it shifts");
    writer.line("// the scroll by the same delta (virtualization-v1.md section 6).");
    writer.line("const std::size_t tessera_anchor =");
    writer.line("    " + table + "_geometry_.index_at_offset(tessera_scroll);");
    writer.open("for (std::size_t tessera_slot = 0; tessera_slot < " + table +
                "_count_; ++tessera_slot)");
    writer.line("tessera::ui::Node* tessera_item = " + table + "_items_[tessera_slot].root;");
    writer.open("if (tessera_item == nullptr)");
    writer.line("continue;");
    writer.close("");
    writer.line("const float tessera_measured =");
    writer.line(std::string("    tessera_item->bounds().") + (horizontal ? "width;" : "height;"));
    writer.open("if (!(tessera_measured > 0.0f))");
    writer.line("continue;");
    writer.close("");
    writer.line("const std::size_t tessera_item_index = " + table + "_first_ + tessera_slot;");
    writer.open("if (tessera_item_index >= tessera_total)");
    writer.line("continue;");
    writer.close("");
    if (grid) {
        writer.line("const std::size_t tessera_index = tessera_item_index / tessera_columns;");
        writer.line(
            "// A row extent is the maximum of its measured items: a smaller measure never");
        writer.line("// shrinks the row (the grid runtime owns the same rule).");
        writer.line("if (" + table + "_geometry_.is_measured(tessera_index) &&");
        writer.line("    " + table +
                    "_geometry_.item_main_size(tessera_index) >= tessera_measured) {");
        writer.line("continue;");
        writer.line("}");
    } else {
        writer.line("const std::size_t tessera_index = tessera_item_index;");
    }
    writer.line("const tessera::ui::virtualization::ItemGeometry::MeasureChange tessera_change =");
    writer.line("    " + table + "_geometry_.refine_measure(tessera_index, tessera_measured);");
    writer.open("if (!tessera_change.changed)");
    writer.line("continue;");
    writer.close("");
    writer.line("const tessera::ui::virtualization::AnchorAdjustment tessera_adjust =");
    writer.line("    tessera::ui::virtualization::reanchor_scroll(");
    writer.line("        tessera_scroll, tessera_viewport, " + table +
                "_geometry_.total_extent(),");
    writer.line("        tessera_anchor, tessera_index, tessera_change.delta);");
    writer.line("tessera_scroll = tessera_adjust.scroll_offset;");
    writer.close("");
    writer.line("// Realize the viewport window expanded by the overscan distance");
    writer.line("// (virtualization-v1.md section 4); apply_virtual_<n>() caps the table.");
    writer.line("float tessera_window_start =");
    writer.line("    tessera_scroll - static_cast<float>(" + base + "overscan_);");
    writer.open("if (tessera_window_start < 0.0f)");
    writer.line("tessera_window_start = 0.0f;");
    writer.close("");
    writer.line("const float tessera_window_extent =");
    writer.line("    tessera_viewport + (tessera_scroll - tessera_window_start) +");
    writer.line("    static_cast<float>(" + base + "overscan_);");
    writer.line("const tessera::ui::virtualization::ItemGeometry::Range tessera_range =");
    writer.line("    " + table +
                "_geometry_.visible_range(tessera_window_start, tessera_window_extent);");
    if (grid) {
        writer.line("std::size_t tessera_first = tessera_range.first * tessera_columns;");
        writer.line("std::size_t tessera_last = tessera_range.last * tessera_columns;");
        writer.open("if (tessera_last > tessera_total)");
        writer.line("tessera_last = tessera_total;");
        writer.close("");
        writer.line("apply_virtual_" + ordinal + "(tessera_first, tessera_last);");
    } else {
        writer.line("apply_virtual_" + ordinal + "(tessera_range.first, tessera_range.last);");
    }
    if (collection.events.on_scroll.present) {
        emit_on_scroll_call(writer, collection, table, horizontal);
    }
    if (collection.events.on_end_reached.present) {
        emit_on_end_reached_call(writer, collection, table, grid);
    }
    writer.line("return true;");
    writer.close("");
    writer.line("");
    return true;
}

} // namespace tessera::compiler::codegen::internal
