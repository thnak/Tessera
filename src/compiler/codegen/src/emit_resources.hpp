// SPDX-License-Identifier: MIT

#pragma once

#include "binding_reads.hpp"
#include "codegen_internal.hpp"
#include "emit_dirty.hpp"
#include "emit_dynamic.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// #656 (ADR-067 item 1, docs/specs/pending-ui-v1.md sections 3.4, 4.2 and 10): the generated half
// of a component's `resources`. Each resource is one `tessera::ui::pending::ResourceView` member
// that the host binds (the fake resource in tests and the Studio mock worker, the data store behind
// `StoreResource` in an app); the boundary and the transition mark read it through the view, so
// the generated code never names the store, never allocates and never waits.
//
// Emitted only for a component with a non-empty `resources` list, which keeps every other
// generated base byte-identical.
namespace tessera::compiler::codegen::internal {

[[nodiscard]] bool uses_resources(const ir::Component& component);

// `res_feed` for `res::feed`: the stem of the generated members (`res_feed_view_`,
// `res_feed_status()`, `bind_res_feed()`).
[[nodiscard]] std::string resource_stem(const ir::Resource& resource);
[[nodiscard]] std::string resource_view_member(const ir::Resource& resource);

// The resources `boundary` covers, in read-slot order (indices into `component.resources`).
[[nodiscard]] std::vector<std::size_t> boundary_reads(const ir::Component& component,
                                                      const ir::Node& boundary);

// The resource a transition-marked set of `target` waits for: the first resource whose key reads
// the state cell. False with `error` set when none does (a transition that waits for nothing) or
// when two marked setters wait for different resources (the mark holds one epoch).
[[nodiscard]] bool transition_resource(const ir::Component& component, std::string_view target,
                                       std::size_t& index, std::string& error);

// The tail of a generated `pending_sync()`: folds `terms` (C++ expressions convertible to
// uint64_t) into one signature and, when it changed, sets the pending dirty cell and schedules the
// update (pending-ui-v1.md section 10.1). Needs the `pending_sig_` member of `emit_pending_fields`.
// With `returns_changed` it declares `const bool pending_changed` for the caller to return and does
// not schedule (the resource path, where `pending_sync()` schedules); without it, it schedules.
void emit_pending_dirty_mark(CppWriter& writer, const std::vector<std::string>& terms,
                             bool returns_changed);

// The service a resource reads from, or null when the component does not declare it.
[[nodiscard]] const ir::Service* resource_service(const ir::Component& component,
                                                  const ir::Resource& resource);
// `sync_resource_keys()` (provider binding by key) of a component with resources.
[[nodiscard]] bool emit_key_sync(CppWriter& writer, const ir::Component& component,
                                 std::string& error);
// The two lines a `flush_update()` starts with when the component has resources.
void emit_resource_sync_line(CppWriter& writer, const ir::Component& component);

void emit_resource_fields(CppWriter& writer, const ir::Component& component);
void emit_resource_declarations(CppWriter& writer, const ir::Component& component);
[[nodiscard]] bool emit_resource_sources(CppWriter& writer, const ir::Component& component,
                                         const DynamicPlan& plan, std::string& error);

} // namespace tessera::compiler::codegen::internal
