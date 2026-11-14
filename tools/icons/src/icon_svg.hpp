// SPDX-License-Identifier: MIT
//
// Static Iconify icon -> the engine's vector path payload (ADR-004, ADR-047 item 8).
//
// The engine's vector content is an embedded SVG asset (PaintKind::Vector, VectorDraw.asset) that
// `tessera::rendering::path::load_svg()` decodes into PathShape geometry for the path rasterizer,
// and that the paint pass tints with VectorDraw.tint -- which is what `currentColor` resolves to at
// paint time. The importer therefore emits exactly that payload: one canonical, standalone SVG
// document per icon, with the Iconify body kept verbatim inside a normalized envelope so the icon
// author's markup is not quietly re-interpreted.
//
// Everything outside the engine's static-icon subset is rejected here instead of being drawn wrong:
// the tag/reference rules mirror `src/rendering/path/svg_prescan.cpp` (which is compiled only with
// the optional ThorVG backend, so the importer carries the same rules for the default build), plus
// the importer is stricter about CSS (`<style>`, `class=`) because the engine would ignore a
// stylesheet and silently render a different icon.

#pragma once

#include "icon_set.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace tessera::icons {

struct SvgOptions {
    // Replace every paint with `currentColor` so the icon takes the text color token. Multi-color
    // icons keep their palette when this is false.
    bool monochrome = false;
};

// Validates one Iconify body against the static-icon subset and trims it. When `monochrome` is set,
// every `fill`/`stroke` value other than `none`, `currentColor` or a `url(...)` reference is
// rewritten to `currentColor`.
[[nodiscard]] std::expected<std::string, std::string> prepare_icon_body(std::string_view body,
                                                                        bool monochrome);

// The standalone SVG document embedded as the icon's vector asset. Bytes are a pure function of the
// icon data and the options: LF endings, fixed attribute order, no timestamps or paths.
[[nodiscard]] std::expected<std::string, std::string> canonical_icon_svg(const IconSource& icon,
                                                                         const SvgOptions& options);

} // namespace tessera::icons
