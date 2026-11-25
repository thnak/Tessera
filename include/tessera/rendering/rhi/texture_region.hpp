// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

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

// --- Buffer to texture copy (RHI 1.17, ADR-072)
// -----------------------------------------------------
//
// `CommandList::copy_buffer_to_texture` copies the bytes of a buffer into a texture region. The
// bytes are the texture's own layout (no conversion), so a shader that produced them writes the
// channel order and transfer function the texture expects.

// Every backend's row-pitch requirement is met by the strictest one (WebGPU's `bytesPerRow`), so
// one rule serves a shader written for any of them. A 4K RGBA8 row (15360 bytes) needs no padding.
inline constexpr std::size_t kTextureCopyRowAlignment = 256;

// Bytes per texel of the formats the RHI defines (one for `R8Unorm`, four otherwise).
[[nodiscard]] constexpr std::size_t texture_format_bytes(TextureFormat format) noexcept {
    return format == TextureFormat::R8Unorm ? 1U : 4U;
}

// The smallest valid `BufferTextureCopy::buffer_row_pitch` for `width` texels of `format`.
[[nodiscard]] constexpr std::size_t aligned_texture_copy_pitch(std::uint32_t width,
                                                               TextureFormat format) noexcept {
    const std::size_t row = static_cast<std::size_t>(width) * texture_format_bytes(format);
    return (row + kTextureCopyRowAlignment - 1U) / kTextureCopyRowAlignment *
           kTextureCopyRowAlignment;
}

struct BufferTextureCopy {
    // The destination rectangle; an empty region copies nothing.
    TextureRegion region;
    // Bytes from the start of the buffer to the region's first texel; a multiple of 4.
    std::size_t buffer_offset = 0;
    // Bytes between the starts of two rows in the buffer; a multiple of `kTextureCopyRowAlignment`
    // no smaller than one row. 0 means `aligned_texture_copy_pitch(region.width, format)`.
    std::size_t buffer_row_pitch = 0;
};

// The one validation of a buffer-to-texture copy, shared by every backend so they refuse the same
// inputs. A region outside `extent` is `std::out_of_range` (as `checked_region_pitch`); a pitch
// that is not a multiple of `kTextureCopyRowAlignment` or is shorter than one row, an offset that
// is not a multiple of 4, or a buffer shorter than `offset + pitch * (height - 1) + one row` is
// `std::invalid_argument`. Returns the effective pitch, or 0 for an empty region (nothing is
// copied). The caller still checks the buffer's usage, the texture's sample and level counts, and
// the state of the command list with its own backend wording.
[[nodiscard]] inline std::size_t checked_buffer_texture_copy(Extent2D extent, TextureFormat format,
                                                             const BufferTextureCopy& copy,
                                                             std::size_t buffer_size) {
    const TextureRegion& region = copy.region;
    if (region.empty()) {
        return 0;
    }
    if (region.x > extent.width || region.width > extent.width - region.x ||
        region.y > extent.height || region.height > extent.height - region.y) {
        throw std::out_of_range("texture copy region lies outside the texture");
    }
    const std::size_t row_bytes =
        static_cast<std::size_t>(region.width) * texture_format_bytes(format);
    const std::size_t pitch = copy.buffer_row_pitch != 0
                                  ? copy.buffer_row_pitch
                                  : aligned_texture_copy_pitch(region.width, format);
    if (pitch < row_bytes || pitch % kTextureCopyRowAlignment != 0) {
        throw std::invalid_argument("texture copy row pitch must be a multiple of " +
                                    std::to_string(kTextureCopyRowAlignment) +
                                    " and at least one row");
    }
    if (copy.buffer_offset % 4U != 0) {
        throw std::invalid_argument("texture copy buffer offset must be a multiple of 4");
    }
    const std::size_t rows_before_last = static_cast<std::size_t>(region.height) - 1U;
    // pitch * rows_before_last + offset + row_bytes must fit the buffer, without overflowing.
    if (copy.buffer_offset > buffer_size || row_bytes > buffer_size - copy.buffer_offset ||
        (rows_before_last != 0 &&
         pitch > (buffer_size - copy.buffer_offset - row_bytes) / rows_before_last)) {
        throw std::invalid_argument("texture copy reads past the end of the buffer");
    }
    return pitch;
}

} // namespace tessera::rendering::rhi
