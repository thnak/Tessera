// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_rasterizer.hpp"

#include <memory>

namespace tessera::rendering::path {

// Backend factory implemented by thorvg_path_rasterizer.cpp. Only compiled into the library when
// TESSERA_ENABLE_THORVG is ON; path_rasterizer.cpp includes this header under the same guard.
[[nodiscard]] std::unique_ptr<PathRasterizer> make_thorvg_path_rasterizer();

} // namespace tessera::rendering::path
