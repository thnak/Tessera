// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/path/path_model.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>

namespace tessera::rendering::path {

enum class RasterError : std::uint8_t {
    invalid_target = 0,  // target is empty, undersized, or has a stride that is not 4-byte aligned
    invalid_path,        // malformed segment stream (e.g. a drawing command before move_to)
    invalid_paint,       // non-positive stroke width or a negative miter limit
    unsupported_backend, // the requested backend was not compiled in
    backend_failure,     // the backend reported a failure
};

struct RasterStats {
    // Shapes submitted by this call; a shape with neither fill nor stroke still counts.
    std::uint32_t shapes = 0;
};

// The seam between Tessera's rendering code and a vector-path rasterizer (ADR-004). Public
// headers never expose a third-party rasterizer type: the only implementation that includes one
// is the backend adapter, so swapping ThorVG for another library does not touch UI code.
//
// Implementations are deterministic for a fixed input and target: single-threaded rasterization,
// no wall clock, no system fonts, no random ids.
class PathRasterizer {
public:
    PathRasterizer() = default;
    virtual ~PathRasterizer() = default;

    PathRasterizer(const PathRasterizer&) = delete;
    PathRasterizer& operator=(const PathRasterizer&) = delete;
    PathRasterizer(PathRasterizer&&) = delete;
    PathRasterizer& operator=(PathRasterizer&&) = delete;

    // Stable, human-readable backend name for diagnostics and goldens.
    [[nodiscard]] virtual std::string_view backend_name() const noexcept = 0;

    // Composites the shapes in order into `target` using source-over blending. The target is
    // caller-owned and already initialized; this method does not clear it. Not thread-safe; use
    // one instance per rasterizing thread.
    [[nodiscard]] virtual std::expected<RasterStats, RasterError>
    rasterize(std::span<const PathShape> shapes, const RasterTarget& target) = 0;
};

enum class PathBackend : std::uint8_t {
    null_backend = 0, // always available; validates input and leaves the pixels untouched
    thorvg = 1,       // available only in builds configured with TESSERA_ENABLE_THORVG=ON
};

// True when this build contains the backend. A false result means the dependency was not enabled,
// not that the backend is broken.
[[nodiscard]] bool path_backend_available(PathBackend backend) noexcept;

// Returns nullptr when the backend was not compiled in; callers must fall back explicitly rather
// than assume a rasterizer exists.
[[nodiscard]] std::unique_ptr<PathRasterizer> make_path_rasterizer(PathBackend backend);

} // namespace tessera::rendering::path
