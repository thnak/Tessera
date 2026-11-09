// SPDX-License-Identifier: MIT

#include "path_validate.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"

#ifdef TESSERA_HAVE_THORVG
#include "thorvg_path_rasterizer.hpp"
#endif

#include <memory>
#include <string_view>

namespace tessera::rendering::path {
namespace {

// Stub backend for tests and non-ThorVG builds. It validates the request exactly like a real
// backend and reports the shape count, but it paints nothing: the seam therefore links everywhere
// and tests can assert plumbing without depending on a rasterizer.
class NullPathRasterizer final : public PathRasterizer {
public:
    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "null";
    }

    [[nodiscard]] std::expected<RasterStats, RasterError>
    rasterize(std::span<const PathShape> shapes, const RasterTarget& target) override {
        if (!target.valid()) {
            return std::unexpected(RasterError::invalid_target);
        }
        for (const PathShape& shape : shapes) {
            if (!validate_path(shape.path)) {
                return std::unexpected(RasterError::invalid_path);
            }
            if (!validate_paint(shape.paint)) {
                return std::unexpected(RasterError::invalid_paint);
            }
        }
        return RasterStats{static_cast<std::uint32_t>(shapes.size())};
    }
};

} // namespace

bool path_backend_available(PathBackend backend) noexcept {
    switch (backend) {
    case PathBackend::null_backend:
        return true;
    case PathBackend::thorvg:
#ifdef TESSERA_HAVE_THORVG
        return true;
#else
        return false;
#endif
    }
    return false;
}

std::unique_ptr<PathRasterizer> make_path_rasterizer(PathBackend backend) {
    switch (backend) {
    case PathBackend::null_backend:
        return std::make_unique<NullPathRasterizer>();
    case PathBackend::thorvg:
#ifdef TESSERA_HAVE_THORVG
        return make_thorvg_path_rasterizer();
#else
        return nullptr;
#endif
    }
    return nullptr;
}

} // namespace tessera::rendering::path
