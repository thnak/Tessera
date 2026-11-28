// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"
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

void emit_resource_fields(CppWriter& writer, const ir::Component& component);
void emit_resource_declarations(CppWriter& writer, const ir::Component& component);
[[nodiscard]] bool emit_resource_sources(CppWriter& writer, const ir::Component& component,
                                         const DynamicPlan& plan, std::string& error);

} // namespace tessera::compiler::codegen::internal
