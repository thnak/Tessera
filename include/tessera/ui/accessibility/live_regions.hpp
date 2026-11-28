// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/a11y_tree.hpp"
#include "tessera/ui/accessibility/announcement_queue.hpp"

#include <cstddef>

namespace tessera::ui::a11y {

// Turns what changed between two consecutive snapshots into announcements for the live regions in
// `next` (docs/specs/accessibility-v1.md section 8). A live region (`A11yNode::live` not `Off`)
// announces its name, or its value when it has no name, when it is new in `next` or that text
// differs from the same node's text in `previous`. Nodes are matched by id.
//
// An empty `previous` announces nothing: the first snapshot is the initial content, which a live
// region does not read out (ARIA announces changes, not the page load). A toast that appears later
// is a new node, so it is announced.
//
// Returns how many announcements the queue accepted (the queue drops duplicates and `Off`).
std::size_t collect_live_announcements(const A11ySnapshot& previous, const A11ySnapshot& next,
                                       accessibility::AnnouncementQueue& queue);

} // namespace tessera::ui::a11y
