// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node_types.hpp"

// Forward declaration of `ui::Node` plus `NodeId`, for headers that only hold or pass a node by
// pointer or reference. `node.hpp` is the most-included header of the tree (docs/architecture/
// god-type-audit-v1.md section 3.1), so a header that does not call into the node declares it here
// and leaves the full class to the translation units that do.
namespace tessera::ui {

class Node;

} // namespace tessera::ui
