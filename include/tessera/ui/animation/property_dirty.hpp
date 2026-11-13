// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/dirty.hpp"
#include "tessera/ui/style/keyframes.hpp"
#include "tessera/ui/style/parser.hpp"

#include <string_view>
#include <vector>

namespace tessera::ui::animation {

bool is_layout_property(std::string_view property);
// Properties an animation must keep running under adaptive quality's last ladder position (#147):
// layout properties (a frozen layout animation would leave the scene half-moved), `transform` and
// `opacity` (the motion and fades a user reads as the interface responding).
bool is_essential_animation_property(std::string_view property);
// True when every declaration in `block` animates a paint-only, non-essential property (a colour or
// shadow fade, say) and there is at least one. Anything mixed with an essential property is
// essential as a whole.
bool is_non_essential_animation(const style::KeyframeBlock& block);
Dirty dirty_for_property(std::string_view property);
Dirty dirty_for_declarations(const std::vector<style::Declaration>& declarations);
// True when changing `property` repaints the whole painted subtree, not only the target node.
// `opacity` is the one such property today: its value composites every descendant (the
// non-isolated effective-opacity rule of docs/design/paint-pass.md section 14.1), so an animated
// ancestor fade must mark the descendants Paint|Render as well. Everything else in the dirty map
// only affects the target's own box.
bool paint_dirties_subtree(std::string_view property);
bool declarations_dirty_paint_subtree(const std::vector<style::Declaration>& declarations);

} // namespace tessera::ui::animation
