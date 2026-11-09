// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_model.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace tessera::rendering::path {

// Bounds for one load. They are enforced against the raw input and the decoded scene tree, so a
// hostile or accidentally huge asset fails with a value instead of exhausting memory or recursion.
struct SvgLimits {
    std::size_t max_input_bytes = 1024U * 1024U; // 1 MiB of SVG source
    std::uint32_t max_shapes = 1024;             // decoded Shape paints (visible or not)
    std::uint32_t max_depth = 32;                // nested group/scene depth
    std::uint32_t max_segments = 65536;          // total path segments in the document
    std::uint32_t max_extent = 16384;            // document width/height upper bound, in units

    [[nodiscard]] bool operator==(const SvgLimits&) const noexcept = default;
};

enum class SvgError : std::uint8_t {
    invalid_argument = 0, // empty input, zero/nonsense limits
    input_too_large,      // span exceeds limits.max_input_bytes
    malformed,            // the SVG loader rejected the bytes or the viewport is unusable
    unsupported,          // text/image/filter/gradient/animation/reference/clip/mask content
    limit_exceeded,       // shape/depth/segment/document-extent budget exceeded
    backend_unavailable,  // built without TESSERA_ENABLE_THORVG: a value, never a link error
    backend_failure,      // allocation or internal ThorVG failure
};

// A decoded static icon: absolutely-positioned path shapes in document coordinates (groups and
// transforms are baked in) plus the resolved document size. `shapes` are in painter order, so
// rasterizing them in order reproduces the SVG stacking.
struct SvgDocument {
    std::vector<PathShape> shapes;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// True when this build contains an SVG backend. When false, `load_svg()` returns
// `SvgError::backend_unavailable`; callers fall back without a special build.
[[nodiscard]] bool svg_backend_available() noexcept;

// Decodes a static SVG (the ThorVG static-icon subset, see docs/design/thorvg-spike/README.md) into
// path geometry. Never throws across this seam: every failure is a value. The returned geometry is
// a pure function of the bytes and the ThorVG version (deterministic; floats are copied, not
// re-rounded). Not thread-safe: call from one thread, like the rest of the path module.
[[nodiscard]] std::expected<SvgDocument, SvgError> load_svg(std::span<const std::uint8_t> bytes,
                                                            SvgLimits limits = {});

[[nodiscard]] std::expected<SvgDocument, SvgError> load_svg(std::span<const char> bytes,
                                                            SvgLimits limits = {});

} // namespace tessera::rendering::path
