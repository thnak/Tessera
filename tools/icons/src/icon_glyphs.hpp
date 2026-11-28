// SPDX-License-Identifier: MIT
//
// `IconGlyph.g.tsx`: the imported icons as `Path` intrinsics, selected by name (issue #530).
//
// **Why TSX and not the embedded SVG assets alone.** The Icon component is TSX, and a `Path`'s `d`
// and `viewBox` must be literals (jsx-intrinsics-v1 section 1), so a component that takes a
// runtime `name` selects among literal `Path` nodes. This file is that selection, generated from
// the same canonical SVG documents the tool embeds, so the two cannot disagree about an icon.
//
// Only icons made of `<path>` elements are representable: anything else (shapes, groups, unknown
// attributes) is listed in the file header and left out instead of being redrawn differently.

#pragma once

#include "icon_types.hpp"

#include <string>
#include <vector>

namespace tessera::icons {

// `files` are the already-built asset documents of `result` (`icons/<set>/<name>.svg`).
[[nodiscard]] std::string build_icon_glyphs_tsx(const ImportResult& result);

} // namespace tessera::icons
