// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/svg_loader.hpp"

#include <cstdint>
#include <expected>
#include <span>

namespace tessera::rendering::path {

// Conservative tag/reference pre-scan of an SVG document. It runs before ThorVG sees the bytes so a
// rejected asset (text, images, gradients, filters, `use`, external references, ...) is rejected
// for a documented, testable reason rather than by loader behavior. Contains no ThorVG code.
[[nodiscard]] std::expected<void, SvgError>
scan_unsupported_svg(std::span<const std::uint8_t> bytes);

} // namespace tessera::rendering::path
