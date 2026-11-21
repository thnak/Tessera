// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace tessera::rendering::rhi {

// A rectangle of texels inside a texture, in whole texels with the origin at the top left (RHI 1.9,
// `Texture::write_region`).
struct TextureRegion {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    [[nodiscard]] bool empty() const noexcept {
        return width == 0 || height == 0;
    }
};

// The one validation of a region write, shared by every backend so they refuse the same inputs:
//  - the region must lie inside `extent` (`std::out_of_range`),
//  - `row_pitch` is bytes between the starts of two source rows; 0 means tightly packed, and a
//    pitch smaller than one region row is refused (`std::invalid_argument`),
//  - an empty region is valid and writes nothing, so a caller that tracks a dirty rectangle need
//    not special-case "nothing changed".
// Returns the effective pitch (a non-empty region always has a non-zero one). The caller still
// checks `data != nullptr` for a non-empty region and rejects mipmapped/multisampled textures with
// its own backend wording.
[[nodiscard]] inline std::size_t checked_region_pitch(Extent2D extent, std::size_t bytes_per_pixel,
                                                      const TextureRegion& region,
                                                      std::size_t row_pitch) {
    if (region.empty()) {
        return row_pitch;
    }
    if (region.x > extent.width || region.width > extent.width - region.x ||
        region.y > extent.height || region.height > extent.height - region.y) {
        throw std::out_of_range("texture region lies outside the texture");
    }
    const std::size_t row_bytes = static_cast<std::size_t>(region.width) * bytes_per_pixel;
    const std::size_t pitch = row_pitch != 0 ? row_pitch : row_bytes;
    if (pitch < row_bytes) {
        throw std::invalid_argument("texture region row pitch is smaller than one row");
    }
    return pitch;
}

} // namespace tessera::rendering::rhi
