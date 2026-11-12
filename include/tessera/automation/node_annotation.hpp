// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace tessera::automation {

// Source-level identity for a scene node, keyed by automation id. A `ui::Node` cannot carry a
// manifest's structural path or component id, so the host stamps them onto snapshot entries
// instead (docs/specs/automation-ids-v1.md section 5.2). Nodes without an entry keep the path the
// host derives from the live tree.
struct NodeAnnotation {
    std::string path;
    std::uint64_t component_id = 0;
};
using NodeAnnotations = std::unordered_map<std::uint64_t, NodeAnnotation>;

} // namespace tessera::automation
