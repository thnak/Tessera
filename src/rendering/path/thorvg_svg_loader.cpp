// SPDX-License-Identifier: MIT
//
// SVG -> path geometry through ThorVG's SVG loader. This is the only Tessera file besides
// thorvg_path_rasterizer.cpp allowed to include a ThorVG header (the configure-time layering gate
// enforces it).
//
// The static-icon subset is decoded by ThorVG and then baked into the path model: groups and
// transforms are composed into absolute coordinates, solid fill/stroke colors and paint opacity are
// folded into the per-shape colors, and everything else is either rejected as unsupported or
// ignored per the documented rules. Rasterizing the result with PathRasterizer therefore needs no
// transform stack.

#include "svg_prescan.hpp"
#include "thorvg_svg_loader.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <thorvg.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace tessera::rendering::path {
namespace {

static_assert(static_cast<int>(tvg::Type::Shape) == 1, "ThorVG Type values are part of the map");
static_assert(static_cast<int>(tvg::StrokeCap::Butt) == 0 &&
                  static_cast<int>(tvg::StrokeCap::Round) == 1 &&
                  static_cast<int>(tvg::StrokeCap::Square) == 2,
              "ThorVG cap order matches LineCap");
static_assert(static_cast<int>(tvg::StrokeJoin::Miter) == 0 &&
                  static_cast<int>(tvg::StrokeJoin::Round) == 1 &&
                  static_cast<int>(tvg::StrokeJoin::Bevel) == 2,
              "ThorVG join order matches LineJoin");

// 2D affine transform, column-vector convention (x' = e11*x + e12*y + e13), matching ThorVG.
struct Affine {
    float m[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
};

[[nodiscard]] Affine compose(const Affine& parent, const tvg::Matrix& local) noexcept {
    Affine result;
    result.m[0] = parent.m[0] * local.e11 + parent.m[1] * local.e21;
    result.m[1] = parent.m[0] * local.e12 + parent.m[1] * local.e22;
    result.m[2] = parent.m[0] * local.e13 + parent.m[1] * local.e23 + parent.m[2];
    result.m[3] = parent.m[3] * local.e11 + parent.m[4] * local.e21;
    result.m[4] = parent.m[3] * local.e12 + parent.m[4] * local.e22;
    result.m[5] = parent.m[3] * local.e13 + parent.m[4] * local.e23 + parent.m[5];
    return result;
}

[[nodiscard]] PointF apply(const Affine& transform, float x, float y) noexcept {
    return PointF{transform.m[0] * x + transform.m[1] * y + transform.m[2],
                  transform.m[3] * x + transform.m[4] * y + transform.m[5]};
}

// Opacity composition matches ThorVG's renderer: 8-bit alpha multiplied per level.
[[nodiscard]] std::uint8_t multiply_alpha(std::uint8_t lhs, std::uint8_t rhs) noexcept {
    constexpr float kScale = 1.0f / 255.0f;
    const float product = static_cast<float>(lhs) * static_cast<float>(rhs) * kScale;
    return static_cast<std::uint8_t>(std::lround(product));
}

struct PictureDeleter {
    void operator()(tvg::Picture* picture) const noexcept {
        if (picture != nullptr) {
            tvg::Paint::rel(picture);
        }
    }
};

struct AccessorDeleter {
    void operator()(tvg::Accessor* accessor) const noexcept {
        delete accessor;
    }
};

struct InitializerGuard {
    bool active = false;
    ~InitializerGuard() {
        if (active) {
            static_cast<void>(tvg::Initializer::term());
        }
    }
};

struct BuildContext {
    explicit BuildContext(const SvgLimits& svg_limits)
        : limits(svg_limits) {}

    const SvgLimits& limits;
    std::vector<PathShape> shapes;
    std::uint32_t shape_count = 0;
    std::uint32_t segment_count = 0;
    SvgError error = SvgError::backend_failure;
    bool failed = false;
};

using ChildrenMap = std::unordered_map<const tvg::Paint*, std::vector<const tvg::Paint*>>;

[[nodiscard]] bool fail(BuildContext& context, SvgError error) {
    context.error = error;
    context.failed = true;
    return false;
}

// Converts one Shape paint; returns false on a decode failure (context.error is set).
[[nodiscard]] bool convert_shape(const tvg::Shape& shape, const Affine& world,
                                 std::uint8_t inherited_alpha, BuildContext& context) {
    if (shape.clip() != nullptr) {
        return fail(context, SvgError::unsupported);
    }
    const tvg::Paint* mask_target = nullptr;
    if (shape.mask(&mask_target) != tvg::MaskMethod::None) {
        return fail(context, SvgError::unsupported);
    }
    if (shape.fill() != nullptr || shape.strokeFill() != nullptr) {
        return fail(context, SvgError::unsupported); // gradient fill/stroke
    }

    ++context.shape_count;
    if (context.shape_count > context.limits.max_shapes) {
        return fail(context, SvgError::limit_exceeded);
    }

    const float determinant = world.m[0] * world.m[4] - world.m[1] * world.m[3];
    if (!std::isfinite(determinant)) {
        return fail(context, SvgError::backend_failure);
    }
    if (determinant == 0.0f) {
        return true; // degenerate transform: the shape paints nothing
    }

    const tvg::PathCommand* commands = nullptr;
    std::uint32_t command_count = 0;
    const tvg::Point* points = nullptr;
    std::uint32_t point_count = 0;
    if (shape.path(&commands, &command_count, &points, &point_count) != tvg::Result::Success ||
        commands == nullptr || points == nullptr) {
        return true; // no geometry to contribute
    }

    Path path;
    path.fill_rule =
        shape.fillRule() == tvg::FillRule::EvenOdd ? FillRule::even_odd : FillRule::non_zero;
    std::uint32_t next_point = 0;
    for (std::uint32_t command_index = 0; command_index < command_count; ++command_index) {
        PathSegment segment;
        switch (commands[command_index]) {
        case tvg::PathCommand::MoveTo:
        case tvg::PathCommand::LineTo: {
            if (next_point >= point_count) {
                return fail(context, SvgError::backend_failure);
            }
            const PointF point = apply(world, points[next_point].x, points[next_point].y);
            segment.kind = commands[command_index] == tvg::PathCommand::MoveTo
                               ? SegmentKind::move_to
                               : SegmentKind::line_to;
            segment.p0 = point;
            ++next_point;
            break;
        }
        case tvg::PathCommand::CubicTo: {
            if (next_point + 2 >= point_count) {
                return fail(context, SvgError::backend_failure);
            }
            segment.kind = SegmentKind::cubic_to;
            segment.p0 = apply(world, points[next_point].x, points[next_point].y);
            segment.p1 = apply(world, points[next_point + 1].x, points[next_point + 1].y);
            segment.p2 = apply(world, points[next_point + 2].x, points[next_point + 2].y);
            next_point += 3;
            break;
        }
        case tvg::PathCommand::Close:
            segment.kind = SegmentKind::close;
            break;
        }
        ++context.segment_count;
        if (context.segment_count > context.limits.max_segments) {
            return fail(context, SvgError::limit_exceeded);
        }
        path.segments.push_back(segment);
    }
    if (path.segments.empty()) {
        return true;
    }

    PaintStyle paint;
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 0;
    if (shape.fill(&r, &g, &b, &a) == tvg::Result::Success && a > 0) {
        paint.fill = Rgba8{r, g, b, multiply_alpha(a, inherited_alpha)};
    }
    const float stroke_width = shape.strokeWidth();
    std::uint8_t sr = 0;
    std::uint8_t sg = 0;
    std::uint8_t sb = 0;
    std::uint8_t sa = 0;
    if (stroke_width > 0.0f && shape.strokeFill(&sr, &sg, &sb, &sa) == tvg::Result::Success &&
        sa > 0) {
        // The seam has no transform stack, so a non-uniform transform is approximated by scaling
        // the stroke width with sqrt(|det|) (documented).
        const float stroke_scale = std::sqrt(std::fabs(determinant));
        StrokeStyle stroke;
        stroke.color = Rgba8{sr, sg, sb, multiply_alpha(sa, inherited_alpha)};
        stroke.width = stroke_width * stroke_scale;
        stroke.cap =
            shape.strokeCap() == tvg::StrokeCap::Round
                ? LineCap::round
                : (shape.strokeCap() == tvg::StrokeCap::Square ? LineCap::square : LineCap::butt);
        stroke.join = shape.strokeJoin() == tvg::StrokeJoin::Round
                          ? LineJoin::round
                          : (shape.strokeJoin() == tvg::StrokeJoin::Bevel ? LineJoin::bevel
                                                                          : LineJoin::miter);
        stroke.miter_limit = shape.strokeMiterlimit();
        paint.stroke = stroke;
    }
    if (!paint.fill.has_value() && !paint.stroke.has_value()) {
        return true;
    }
    context.shapes.push_back(PathShape{std::move(path), paint});
    return true;
}

[[nodiscard]] bool walk(const tvg::Paint& paint, const Affine& parent_world,
                        std::uint8_t parent_alpha, std::uint32_t depth, BuildContext& context,
                        const ChildrenMap& children) {
    if (depth > context.limits.max_depth) {
        return fail(context, SvgError::limit_exceeded);
    }
    if (!paint.visible()) {
        return true; // display:none / visibility:hidden subtrees are ignored
    }
    const Affine world = compose(parent_world, const_cast<tvg::Paint&>(paint).transform());
    const std::uint8_t alpha = multiply_alpha(parent_alpha, paint.opacity());

    switch (paint.type()) {
    case tvg::Type::Scene: {
        const auto found = children.find(&paint);
        if (found != children.end()) {
            for (const tvg::Paint* child : found->second) {
                if (!walk(*child, world, alpha, depth + 1, context, children)) {
                    return false;
                }
            }
        }
        return true;
    }
    case tvg::Type::Shape:
        return convert_shape(static_cast<const tvg::Shape&>(paint), world, alpha, context);
    case tvg::Type::Text:
    case tvg::Type::Picture:
        return fail(context, SvgError::unsupported);
    default:
        return true; // unknown node kinds carry no geometry we could represent
    }
}

} // namespace

std::expected<SvgDocument, SvgError> load_svg_thorvg(std::span<const std::uint8_t> bytes,
                                                     const SvgLimits& limits) {
    if (bytes.empty() || limits.max_input_bytes == 0U || limits.max_shapes == 0U ||
        limits.max_depth == 0U || limits.max_segments == 0U || limits.max_extent == 0U) {
        return std::unexpected(SvgError::invalid_argument);
    }
    if (bytes.size() > limits.max_input_bytes ||
        bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(SvgError::input_too_large);
    }

    try {
        const auto scanned = scan_unsupported_svg(bytes);
        if (!scanned.has_value()) {
            return std::unexpected(scanned.error());
        }

        InitializerGuard initializer;
        if (tvg::Initializer::init(0) != tvg::Result::Success) {
            return std::unexpected(SvgError::backend_failure);
        }
        initializer.active = true;

        std::unique_ptr<tvg::Picture, PictureDeleter> picture(tvg::Picture::gen());
        if (!picture) {
            return std::unexpected(SvgError::backend_failure);
        }
        const auto loaded =
            picture->load(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::uint32_t>(bytes.size()), "svg", nullptr, true);
        if (loaded != tvg::Result::Success) {
            return std::unexpected(loaded == tvg::Result::FailedAllocation
                                       ? SvgError::backend_failure
                                       : SvgError::malformed);
        }

        float width = 0.0f;
        float height = 0.0f;
        if (picture->size(&width, &height) != tvg::Result::Success || !(width > 0.0f) ||
            !(height > 0.0f) || !std::isfinite(width) || !std::isfinite(height)) {
            return std::unexpected(SvgError::malformed);
        }
        if (width > static_cast<float>(limits.max_extent) ||
            height > static_cast<float>(limits.max_extent)) {
            return std::unexpected(SvgError::limit_exceeded);
        }

        // Enumerate every paint, then reconstruct parent/child links from Scene::paints(). The
        // public API gives no root child list, so root children are the paints that no scene owns.
        std::vector<const tvg::Paint*> all_paints;
        std::unordered_map<const tvg::Paint*, std::vector<const tvg::Paint*>> children;
        std::unordered_set<const tvg::Paint*> owned;
        std::unique_ptr<tvg::Accessor, AccessorDeleter> accessor(tvg::Accessor::gen());
        if (!accessor) {
            return std::unexpected(SvgError::backend_failure);
        }
        const auto visited = accessor->set(
            picture.get(),
            [&all_paints, &children, &owned](const tvg::Paint* paint, void*) {
                all_paints.push_back(paint);
                if (paint->type() == tvg::Type::Scene) {
                    auto& list = children[paint];
                    for (const tvg::Paint* child :
                         static_cast<const tvg::Scene*>(paint)->paints()) {
                        list.push_back(child);
                        owned.insert(child);
                    }
                }
                return true;
            },
            nullptr);
        if (visited != tvg::Result::Success) {
            return std::unexpected(SvgError::backend_failure);
        }

        std::vector<const tvg::Paint*> roots;
        for (const tvg::Paint* paint : all_paints) {
            if (paint != picture.get() && owned.find(paint) == owned.end()) {
                roots.push_back(paint);
            }
        }

        BuildContext context(limits);
        for (const tvg::Paint* root : roots) {
            if (!walk(*root, Affine{}, 255, 0, context, children)) {
                return std::unexpected(context.error);
            }
        }

        SvgDocument document;
        document.width = static_cast<std::uint32_t>(std::ceil(width));
        document.height = static_cast<std::uint32_t>(std::ceil(height));
        if (document.width == 0U || document.height == 0U) {
            return std::unexpected(SvgError::malformed);
        }
        document.shapes = std::move(context.shapes);
        return document;
    } catch (...) {
        return std::unexpected(SvgError::backend_failure);
    }
}

} // namespace tessera::rendering::path
