// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/component.hpp"

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

namespace tessera::compiler::ir {

// A `Suspense` boundary and the suspending reads it covers (docs/specs/pending-ui-v1.md section
// 4.2). `resources` are indices into `Component::resources`, in resource declaration order, so
// the position in the list is the read slot the boundary registers for that resource.
struct BoundaryReads {
    const Node* boundary = nullptr;
    std::vector<std::size_t> resources;
};

struct SuspenseReads {
    // One entry per Suspense node, in the order the dynamic plan visits them (children, then the
    // fallback), which is the order codegen numbers boundaries in.
    std::vector<BoundaryReads> boundaries;
    // Suspending resources (`suspend`) with no covering read: no `res::<name>.*` read inside any
    // boundary of the component, or a `.data` read outside every boundary. Ascending index.
    std::vector<std::size_t> uncovered;
};

// Which boundary covers which suspending resource. A node reads a resource through a
// `res::<name>.status|data|error` reference in any of its expressions; the **nearest** enclosing
// Suspense (its child subtree, not its fallback) covers it. Only a `.data` read must be covered:
// `.status` and `.error` are always readable, so they may also appear outside every boundary.
// Shared by the validator, the evaluator and codegen so an accepted module and a runnable module
// cannot disagree.
[[nodiscard]] SuspenseReads collect_suspense_reads(const Component& component);

// The resource a transition-marked set of `state_id` waits for: the first resource whose key reads
// that state cell (the id itself or a dotted member of it). Nullopt when no key does, in which case
// the transition would wait for nothing.
[[nodiscard]] std::optional<std::size_t> resource_keyed_by(const Component& component,
                                                           std::string_view state_id);

} // namespace tessera::compiler::ir
