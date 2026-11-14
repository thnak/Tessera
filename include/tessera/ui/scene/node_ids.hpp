// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/node.hpp"

namespace tessera::ui {

// Generated components add `node_id_base_` to every NodeId they build (issue #295). A generated
// component bakes the uiId of its nodes into its code, so without a base every instance of the same
// component would build identical ids, and the focus manager, input capture and click synthesis,
// which look nodes up by id, would resolve to the first instance. The owner sets
// `child.node_id_base_ = host_id * kNodeIdBaseStride` before `child.build(host)`; the host id is
// unique in the owner and already includes the owner's own base, so nested instances stay distinct.
// The multiplier is odd and wide, so bases of different hosts rarely overlap with a baked uiId.
inline constexpr NodeId kNodeIdBaseStride = 0x9E3779B97F4A7C15ull;

} // namespace tessera::ui
