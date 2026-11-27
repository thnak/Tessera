// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/component.hpp"

#include <cstddef>
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
    // Suspending resources (`suspend`) with no covering read: no `.data` read inside any boundary
    // of the component, or a `.data` read outside every boundary. Ascending resource index.
    std::vector<std::size_t> uncovered;
};

// Which boundary covers which suspending resource. A node reads a resource through a
// `res::<name>.data` reference in any of its expressions; the **nearest** enclosing Suspense (its
// child subtree, not its fallback) covers the read. Only `.data` suspends: `.status` and `.error`
// are always readable. Shared by the validator, the evaluator and codegen so an accepted module and
// a runnable module cannot disagree.
[[nodiscard]] SuspenseReads collect_suspense_reads(const Component& component);

} // namespace tessera::compiler::ir
