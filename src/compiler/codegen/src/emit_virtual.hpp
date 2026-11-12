// SPDX-License-Identifier: MIT

#pragma once

#include "emit_dynamic.hpp"

#include <cstdint>
#include <optional>
#include <string>
// IR -> generated C++ for VirtualList/VirtualGrid item templates (ir-v1.md section 3.5,
// virtualization-v1.md section 10):
//
// - the collection node is an ordinary fixed container node; the item template is not a child, so
//   the bounded realized window is attached to it by apply_virtual_<n>();
// - the window math (prefix sums, estimates, scroll anchoring) belongs to the runtime
//   (`tessera::ui::virtualization::ItemGeometry`), so the generated method takes the half-open
//   [first, last) range the host computed and never re-implements geometry;
// - the item table, keyed reuse, NodeId slots and the per-item content hash reuse the keyed-list
//   emission shape (emit_list.cpp), sized to the applied window (no fixed cap).
namespace tessera::compiler::codegen::internal {

// Resolves and validates the geometry props of one virtual collection. `itemSize`/`estimateSize`/
// `gap`/`rowHeight` accept a number literal or a numeric state/prop reference (the re-seed path
// re-evaluates the latter). `orientation`, `columns`, `overscan` and `columnWidth` accept a literal
// (emitted unchanged) or a component prop reference (`props.<name>`), which becomes a mutable
// member the generated scroll entry point refreshes at sync time. Unsupported forms are
// TESSERA_CODEGEN_UNSUPPORTED.
[[nodiscard]] bool virtual_geometry_spec(const ir::Component& component, const ir::Node& collection,
                                         VirtualGeometrySpec& out, std::string& error);

// The generated member-name prefix of one item table: `list_<n>_` for an eager keyed list,
// `virtual_<n>_` for a virtual collection window (both share the Item_<n> shape).
[[nodiscard]] std::string item_table_prefix(const ListTemplate& list);

// The integer value of a literal expression, or nullopt for a dynamic/absent value.
[[nodiscard]] std::optional<std::int64_t> virtual_integer_literal(const ir::Expr* expr);

// Resolves and validates the collection-level events of every virtual collection in the plan:
// `onScroll` maps to a handler/binding taking the scene `ScrollEvent` payload (or no parameter at
// all) and `onEndReached` to one taking no payload. Invalid forms are TESSERA_CODEGEN_UNSUPPORTED.
// Called once after the dynamic plan is collected; fills `ListTemplate::events`.
[[nodiscard]] bool collect_virtual_events(const ir::Component& component,
                                          const std::vector<const ir::Binding*>& bindings,
                                          DynamicPlan& plan, std::string& error);

// Emits the generated geometry members of one virtual collection into the component struct: the
// effective estimate, the gap, the declared fixed extent and the overscan/axis/columns
// (virtualization-v1.md section 10; the estimate default is 32 px). A literal structural option is
// emitted exactly as before; a prop-driven one becomes a mutable member seeded with the default.
void emit_virtual_geometry_fields(CppWriter& writer, const ListTemplate& collection);

// Emits the sync-time refresh of every prop-driven structural option (`orientation`, `columns`,
// `overscan`, `columnWidth`) into `scroll_virtual_<n>()`, before the row/window math. It reads the
// generated props field once per call (no allocation) and clamps to the runtime contract: an
// unknown orientation is vertical, a negative overscan becomes 0, a non-positive column count
// becomes the column-width mode 0. Emits nothing for a literal-only collection, so those goldens
// never move.
void emit_virtual_option_refresh(CppWriter& writer, const ListTemplate& collection);

// Emits `<Name>::update_virtual_geometry_<n>(scroll_offset, viewport_extent)`, the re-seed path: it
// re-evaluates the geometry inputs, hash-compares them against the last seed (so a per-frame call
// costs one FNV-1a pass and no rebuild), keeps measurements an estimate/gap change cannot
// invalidate (a changed fixed extent drops all of them), and re-anchors the caller's offset so the
// item at the viewport start keeps its viewport position, clamped to `[0, total - viewport]`.
void emit_update_virtual_geometry(CppWriter& writer, const ir::Component& component,
                                  const ListTemplate& collection);

// Emits `<Name>::apply_virtual_<n>(first, last)`: detaches the current window, reconciles it by key
// against the bounded table, attaches item nodes with absolute-index NodeId slots, then runs the
// hash-compared per-item content pass.
[[nodiscard]] bool emit_apply_virtual(CppWriter& writer, const ir::Component& component,
                                      const ListTemplate& collection, std::string& error);

// Emits `<Name>::scroll_virtual_<n>(scroll_offset, viewport_extent)`: the generated scroll entry
// point. It syncs the geometry count, runs the re-seed path (a geometry prop/state change
// re-anchors the offset in place), refines the prefix sums from the realized items' measured
// main-axis bounds, re-anchors again with the section 6 rule, applies the overscanned realized
// window, then dispatches `onScroll` and the edge-triggered, distance-throttled `onEndReached`.
[[nodiscard]] bool emit_virtual_scroll(CppWriter& writer, const ir::Component& component,
                                       const ListTemplate& collection, std::string& error);

} // namespace tessera::compiler::codegen::internal
