// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <optional>

// Internal helpers between the paint pass and the style property spellings. Not installed.
namespace tessera::ui::paint::detail {

namespace rhi = tessera::rendering::rhi;

// Parses a px or bare-number length. Returns nullopt when the property is missing, uses another
// unit (%, em, ...), or is not a finite number.
std::optional<float> length_value(const style::ComputedStyle& style, const char* property);

// Clamped to 0..1, default 1 when missing or unparseable.
float opacity_value(const style::ComputedStyle& style);

// Resolves `property` to a color. Missing values and unparseable text return nullopt;
// `currentColor` resolves to `current_color`.
std::optional<rhi::Color> color_value(const style::ComputedStyle& style, const char* property,
                                      const rhi::Color& current_color);

// The `color` property; falls back to the CSS initial (black) when missing or invalid.
rhi::Color current_color(const style::ComputedStyle& style);

// Width in px of one border side, or 0 when the side is missing, `none`, `hidden`, or invalid.
float border_width(const style::ComputedStyle& style, const char* width_property,
                   const char* style_property);

// The shared corner radius when all four radii are equal, else nullopt. Missing radii count as 0.
std::optional<float> uniform_radius(const style::ComputedStyle& style);

// True when the element clips its children: `overflow-x` or `overflow-y` is any value other than
// `visible` (hidden/clip/scroll/auto). The paint pass uses one clip rect for both axes.
bool clips_children(const style::ComputedStyle& style);

// Overflow clip for `rect` (the parent's border box) from the four `border-*-radius` properties,
// per-corner and clamped to half the smaller side. A zero-radius result is a rectangular clip.
RoundedClip corner_clip(const style::ComputedStyle& style, const rhi::Rect& rect);

} // namespace tessera::ui::paint::detail
