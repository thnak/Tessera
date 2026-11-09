// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/svg_loader.hpp"

namespace tessera::rendering::path {

// Real SVG decode implemented by thorvg_svg_loader.cpp. Only that file includes a ThorVG header;
// svg_loader.cpp dispatches here when the dependency is enabled.
[[nodiscard]] std::expected<SvgDocument, SvgError>
load_svg_thorvg(std::span<const std::uint8_t> bytes, const SvgLimits& limits);

} // namespace tessera::rendering::path
