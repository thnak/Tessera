// SPDX-License-Identifier: MIT

#include "tessera/rendering/path/path_texture_key.hpp"

#include <cmath>
#include <cstdint>
#include <limits>

namespace tessera::rendering::path {
namespace {

[[nodiscard]] std::uint32_t pack_rgba(Rgba8 color) noexcept {
    return static_cast<std::uint32_t>(color.r) | (static_cast<std::uint32_t>(color.g) << 8U) |
           (static_cast<std::uint32_t>(color.b) << 16U) |
           (static_cast<std::uint32_t>(color.a) << 24U);
}

// Quantizes a paint scalar to thousandths for the key. Non-positive and NaN inputs become 0, which
// the cache later rejects as an invalid request rather than caching an ambiguous entry.
[[nodiscard]] std::uint32_t milli(float value) noexcept {
    if (!(value > 0.0f)) {
        return 0;
    }
    const double scaled = std::round(static_cast<double>(value) * 1000.0);
    constexpr double kMax = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
    if (scaled >= kMax) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(scaled);
}

} // namespace

PathTextureKey make_path_texture_key(std::uint64_t path_id, std::uint32_t path_version,
                                     std::uint32_t width, std::uint32_t height,
                                     std::uint32_t scale_milli, const PathShape& shape) noexcept {
    PathTextureKey key;
    key.path_id = path_id;
    key.path_version = path_version;
    key.width = width;
    key.height = height;
    key.scale_milli = scale_milli;
    key.fill_rule = shape.path.fill_rule;
    if (shape.paint.fill.has_value()) {
        key.fill.present = true;
        key.fill.rgba = pack_rgba(*shape.paint.fill);
    }
    if (shape.paint.stroke.has_value()) {
        key.stroke.present = true;
        key.stroke.rgba = pack_rgba(shape.paint.stroke->color);
        key.stroke.width_milli = milli(shape.paint.stroke->width);
        key.stroke.miter_milli = milli(shape.paint.stroke->miter_limit);
        key.stroke.cap = shape.paint.stroke->cap;
        key.stroke.join = shape.paint.stroke->join;
    }
    return key;
}

} // namespace tessera::rendering::path
