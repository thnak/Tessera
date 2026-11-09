// SPDX-License-Identifier: MIT
//
// ThorVG-backed PathRasterizer. This is the only Tessera file allowed to include a ThorVG header;
// the public seam stays free of third-party types (enforced by the configure-time layering gate).
//
// ThorVG v1.1.3 ships CPU, GL and WebGPU engines only - there is no Vulkan engine - so the V1
// production path is: rasterize on the CPU into an RGBA8 buffer here, then upload that buffer
// through the Tessera RHI as a texture.
//
// The initializer runs with zero worker threads, which makes the scheduler run render tasks
// inline on the calling thread: rasterization is then single-threaded and deterministic.

#include "path_validate.hpp"
#include "thorvg_path_rasterizer.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string_view>
#include <thorvg.h>

namespace tessera::rendering::path {
namespace {

constexpr float kTwoThirds = 2.0f / 3.0f;

[[nodiscard]] bool ok(tvg::Result result) noexcept {
    return result == tvg::Result::Success;
}

[[nodiscard]] tvg::FillRule to_tvg(FillRule rule) noexcept {
    return rule == FillRule::even_odd ? tvg::FillRule::EvenOdd : tvg::FillRule::NonZero;
}

[[nodiscard]] tvg::StrokeCap to_tvg(LineCap cap) noexcept {
    switch (cap) {
    case LineCap::round:
        return tvg::StrokeCap::Round;
    case LineCap::square:
        return tvg::StrokeCap::Square;
    case LineCap::butt:
        break;
    }
    return tvg::StrokeCap::Butt;
}

[[nodiscard]] tvg::StrokeJoin to_tvg(LineJoin join) noexcept {
    switch (join) {
    case LineJoin::round:
        return tvg::StrokeJoin::Round;
    case LineJoin::bevel:
        return tvg::StrokeJoin::Bevel;
    case LineJoin::miter:
        break;
    }
    return tvg::StrokeJoin::Miter;
}

// ThorVG 1.1.3 has no quadratic path command. Promoting a quadratic to a cubic is exact:
//   c1 = from + 2/3 * (control - from)
//   c2 = end  + 2/3 * (control - end)
[[nodiscard]] bool append_quad(tvg::Shape& shape, PointF from, const PathSegment& segment) {
    const PointF c1{from.x + kTwoThirds * (segment.p0.x - from.x),
                    from.y + kTwoThirds * (segment.p0.y - from.y)};
    const PointF c2{segment.p1.x + kTwoThirds * (segment.p0.x - segment.p1.x),
                    segment.p1.y + kTwoThirds * (segment.p0.y - segment.p1.y)};
    return ok(shape.cubicTo(c1.x, c1.y, c2.x, c2.y, segment.p1.x, segment.p1.y));
}

// Translates the seam's segment stream onto a ThorVG shape. The caller has already validated the
// stream, so a failure here is a backend failure rather than a caller error.
[[nodiscard]] bool append_path(tvg::Shape& shape, const Path& path) {
    PointF current;
    bool in_subpath = false;
    for (const PathSegment& segment : path.segments) {
        switch (segment.kind) {
        case SegmentKind::move_to:
            if (!ok(shape.moveTo(segment.p0.x, segment.p0.y))) {
                return false;
            }
            current = segment.p0;
            in_subpath = true;
            break;
        case SegmentKind::line_to:
            if (!in_subpath || !ok(shape.lineTo(segment.p0.x, segment.p0.y))) {
                return false;
            }
            current = segment.p0;
            break;
        case SegmentKind::quad_to:
            if (!in_subpath || !append_quad(shape, current, segment)) {
                return false;
            }
            current = segment.p1;
            break;
        case SegmentKind::cubic_to:
            if (!in_subpath || !ok(shape.cubicTo(segment.p0.x, segment.p0.y, segment.p1.x,
                                                 segment.p1.y, segment.p2.x, segment.p2.y))) {
                return false;
            }
            current = segment.p2;
            break;
        case SegmentKind::close:
            if (!in_subpath || !ok(shape.close())) {
                return false;
            }
            in_subpath = false;
            break;
        }
    }
    return true;
}

[[nodiscard]] bool apply_paint(tvg::Shape& shape, const PaintStyle& paint) {
    if (paint.fill.has_value()) {
        const Rgba8& color = *paint.fill;
        if (!ok(shape.fill(color.r, color.g, color.b, color.a))) {
            return false;
        }
    }
    if (paint.stroke.has_value()) {
        const StrokeStyle& stroke = *paint.stroke;
        if (!ok(shape.strokeWidth(stroke.width)) ||
            !ok(shape.strokeFill(stroke.color.r, stroke.color.g, stroke.color.b, stroke.color.a)) ||
            !ok(shape.strokeCap(to_tvg(stroke.cap))) ||
            !ok(shape.strokeJoin(to_tvg(stroke.join))) ||
            !ok(shape.strokeMiterlimit(stroke.miter_limit))) {
            return false;
        }
    }
    return true;
}

class ThorVgPathRasterizer final : public PathRasterizer {
public:
    ThorVgPathRasterizer() noexcept
        : initialized_(tvg::Initializer::init(0) == tvg::Result::Success) {}

    ~ThorVgPathRasterizer() override {
        if (initialized_) {
            static_cast<void>(tvg::Initializer::term());
        }
    }

    [[nodiscard]] std::string_view backend_name() const noexcept override {
        return "thorvg-cpu";
    }

    [[nodiscard]] std::expected<RasterStats, RasterError>
    rasterize(std::span<const PathShape> shapes, const RasterTarget& target) override {
        if (!initialized_) {
            return std::unexpected(RasterError::backend_failure);
        }
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

        tvg::SwCanvas* canvas = tvg::SwCanvas::gen();
        if (canvas == nullptr) {
            return std::unexpected(RasterError::backend_failure);
        }

        // ABGR8888S is ThorVG's un-premultiplied 32-bit layout: on little-endian hosts the bytes
        // are R,G,B,A, which is exactly the seam's straight-alpha RGBA8 contract.
        auto* pixels = reinterpret_cast<std::uint32_t*>(target.pixels.data());
        auto outcome = [&]() -> std::expected<RasterStats, RasterError> {
            if (!ok(canvas->target(pixels, target.stride_bytes / 4U, target.width, target.height,
                                   tvg::ColorSpace::ABGR8888S))) {
                return std::unexpected(RasterError::backend_failure);
            }

            std::uint32_t count = 0;
            for (const PathShape& input : shapes) {
                ++count;
                if (!input.paint.fill.has_value() && !input.paint.stroke.has_value()) {
                    continue;
                }
                tvg::Shape* shape = tvg::Shape::gen();
                if (shape == nullptr) {
                    return std::unexpected(RasterError::backend_failure);
                }
                // A successful add() transfers ownership to the canvas, so only a shape that was
                // not added may be released here.
                if (!append_path(*shape, input.path) || !apply_paint(*shape, input.paint) ||
                    !ok(shape->fillRule(to_tvg(input.path.fill_rule))) || !ok(canvas->add(shape))) {
                    static_cast<void>(tvg::Paint::rel(shape));
                    return std::unexpected(RasterError::backend_failure);
                }
            }

            if (!ok(canvas->draw()) || !ok(canvas->sync())) {
                return std::unexpected(RasterError::backend_failure);
            }
            return RasterStats{count};
        }();

        // The canvas owns every shape added to it, so deleting it releases the paints too.
        delete canvas;
        return outcome;
    }

private:
    bool initialized_ = false;
};

} // namespace

std::unique_ptr<PathRasterizer> make_thorvg_path_rasterizer() {
    return std::make_unique<ThorVgPathRasterizer>();
}

} // namespace tessera::rendering::path
