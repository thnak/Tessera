// SPDX-License-Identifier: MIT
//
// Generated-file builders for the Iconify import. Every builder returns the exact bytes written to
// disk: fixed order, LF endings, no timestamps and no absolute paths, so two runs over the same
// pinned input produce identical files.

#pragma once

#include "icon_types.hpp"

#include <string>
#include <vector>

namespace tessera::icons {

// `icons.g.hpp`: the accessors the `Icon` component will consume (name -> embedded asset, viewBox
// size, monochrome flag).
[[nodiscard]] std::string build_icons_header(const ImportResult& result);

// `icons.manifest`: the manifest `tessera_embed_assets(<target> MANIFEST ...)` reads.
[[nodiscard]] std::string build_embed_manifest(const ImportResult& result);

// `ICON_LICENSES.txt`: per-set license, copyright holder and, for CC-BY, the required attribution.
[[nodiscard]] std::string build_attribution(const std::vector<AttributionSet>& sets);

} // namespace tessera::icons
