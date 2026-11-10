// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <optional>
#include <string_view>

namespace tessera::ui::style {

// Resolves the CSS `transform` and `transform-origin` values into the matrix a Node applies around
// its own box origin (Node::set_transform): translate(origin) * M * translate(-origin), with M the
// functions multiplied left to right as CSS does. `width`/`height` are the node's border box in px
// and resolve percentages (translate and the origin).
//
// Supported functions: translate/translateX/translateY (px, %), scale/scaleX/scaleY (numbers),
// rotate (deg, rad, grad, turn), skew/skewX/skewY (angles) and matrix(a, b, c, d, e, f).
// `none` yields the identity. Any unknown function or malformed argument makes the whole value
// invalid (nullopt), which callers treat as "no transform", like an invalid CSS declaration.
// `origin` accepts one or two of left/center/right/top/bottom/px/% (default `50% 50%`).
[[nodiscard]] std::optional<rendering::rhi::Mat3>
compute_transform(std::string_view transform, std::string_view origin, float width, float height);

} // namespace tessera::ui::style
