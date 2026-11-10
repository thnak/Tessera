// SPDX-License-Identifier: MIT

#pragma once

#include "emit_dynamic.hpp"

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
//   emission shape (emit_list.cpp), bounded by kMaxGeneratedVirtualItems.
namespace tessera::compiler::codegen::internal {

// The generated member-name prefix of one item table: `list_<n>_` for an eager keyed list,
// `virtual_<n>_` for a virtual collection window (both share the Item_<n> shape).
[[nodiscard]] std::string item_table_prefix(const ListTemplate& list);

// Rejects the geometry props this slice cannot emit (a non-literal itemSize/estimateSize/...).
// Called by the plan walk so a failing component reports one deterministic error.
[[nodiscard]] bool validate_virtual_geometry(const ir::Node& collection, std::string& error);

// Emits the generated `static constexpr` geometry constants of one virtual collection into the
// component struct (virtualization-v1.md section 10; the estimate default is 32 px).
void emit_virtual_geometry_constants(CppWriter& writer, const ListTemplate& collection);

// Emits `<Name>::apply_virtual_<n>(first, last)`: detaches the current window, reconciles it by key
// against the bounded table, attaches item nodes with absolute-index NodeId slots, then runs the
// hash-compared per-item content pass.
[[nodiscard]] bool emit_apply_virtual(CppWriter& writer, const ir::Component& component,
                                      const ListTemplate& collection, std::string& error);

} // namespace tessera::compiler::codegen::internal
