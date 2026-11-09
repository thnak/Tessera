// SPDX-License-Identifier: MIT
//
// SVG decode -> path geometry contract: subset geometry (shapes, groups, transforms, viewBox,
// fill/stroke/opacity), the reject-vs-ignore rules, bounds, determinism across two loads, and
// deterministic rasterized bytes at 1x and 2x (checked-in FNV, so a second process and CI verify
// the same bytes). Runs in every configuration: with ThorVG OFF it asserts the clear
// `backend_unavailable` value instead of a link error.

#include "path_texture_cache_test_fakes.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/path_texture_key.hpp"
#include "tessera/rendering/path/svg_loader.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace path = tessera::rendering::path;

using tessera::test::path_texture::fake_texture;
using tessera::test::path_texture::FakeDevice;
using tessera::test::path_texture::fnv1a;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "svg_loader: " << message << "\n";
    }
    return condition;
}

constexpr std::uint32_t kIconSize = 64;

// FNV-1a of the rasterized stroke_path.svg at 1x (64x64) and of the PathTextureCache 2x upload.
// Checked in so any process producing different bytes fails; update only alongside a deliberate
// fixture/rasterizer change.
constexpr std::uint64_t kStrokePathFnv1x = 0xaf7e9adbb9f450e3ULL;
constexpr std::uint64_t kStrokePathFnv2x = 0x4363862c336bf9c3ULL;

[[nodiscard]] bool near(float lhs, float rhs) {
    return std::fabs(lhs - rhs) <= 1.0e-4f;
}

[[nodiscard]] std::vector<std::uint8_t> read_fixture(std::string_view name) {
    const std::string path = std::string(TESSERA_SVG_FIXTURE_DIR) + "/" + std::string(name);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "svg_loader: cannot open fixture " << path << "\n";
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

[[nodiscard]] std::expected<path::SvgDocument, path::SvgError>
load_fixture(std::string_view name, path::SvgLimits limits = {}) {
    const auto bytes = read_fixture(name);
    if (bytes.empty()) {
        return std::unexpected(path::SvgError::malformed);
    }
    return path::load_svg(std::span<const std::uint8_t>(bytes.data(), bytes.size()), limits);
}

[[nodiscard]] bool is_segment(const path::PathSegment& segment, path::SegmentKind kind, float x,
                              float y) {
    return segment.kind == kind && near(segment.p0.x, x) && near(segment.p0.y, y);
}

[[nodiscard]] bool is_rect_outline(const path::Path& path, float left, float top, float right,
                                   float bottom) {
    return path.segments.size() == 5 &&
           is_segment(path.segments[0], path::SegmentKind::move_to, left, top) &&
           is_segment(path.segments[1], path::SegmentKind::line_to, right, top) &&
           is_segment(path.segments[2], path::SegmentKind::line_to, right, bottom) &&
           is_segment(path.segments[3], path::SegmentKind::line_to, left, bottom) &&
           path.segments[4].kind == path::SegmentKind::close;
}

bool backend_behavior() {
    if (path::svg_backend_available()) {
        return expect(path::load_svg(std::span<const char>()).error() ==
                          path::SvgError::invalid_argument,
                      "empty input is invalid_argument even when the backend is available");
    }
    const auto result = path::load_svg(std::span<const char>());
    return expect(!result.has_value() && result.error() == path::SvgError::backend_unavailable,
                  "without ThorVG load_svg returns backend_unavailable instead of failing to link");
}

bool icon_shapes_geometry() {
    const auto document = load_fixture("icon_shapes.svg");
    if (!expect(document.has_value(), "icon_shapes loads")) {
        return false;
    }
    bool ok = expect(document->width == kIconSize && document->height == kIconSize,
                     "icon_shapes keeps its viewBox size") &&
              expect(document->shapes.size() == 2, "icon_shapes decodes two shapes");
    if (!ok) {
        return false;
    }
    const auto& rect = document->shapes[0];
    ok = expect(is_rect_outline(rect.path, 8.0f, 8.0f, 32.0f, 32.0f),
                "rect decodes to an axis-aligned outline") &&
         expect(rect.paint.fill.has_value() && rect.paint.fill->r == 0x33 &&
                    rect.paint.fill->g == 0x66 && rect.paint.fill->b == 0xcc &&
                    rect.paint.fill->a == 255,
                "rect fill color is preserved") &&
         expect(!rect.paint.stroke.has_value(), "rect without stroke gets no stroke") && ok;
    const auto& circle = document->shapes[1];
    std::uint32_t cubics = 0;
    for (const path::PathSegment& segment : circle.path.segments) {
        if (segment.kind == path::SegmentKind::cubic_to) {
            ++cubics;
        }
    }
    ok = expect(circle.path.segments.size() == 6 && cubics == 4,
                "circle decodes to four cubic arcs") &&
         expect(circle.paint.fill.has_value() && circle.paint.fill->r == 0xcc &&
                    circle.paint.fill->g == 0x66 && circle.paint.fill->b == 0x33,
                "circle fill color is preserved") &&
         expect(circle.paint.fill.has_value() && circle.paint.fill->a >= 126 &&
                    circle.paint.fill->a <= 128,
                "fill-opacity 0.5 folds into the fill alpha") &&
         ok;
    return ok;
}

bool group_transform_geometry() {
    const auto document = load_fixture("group_transform.svg");
    if (!expect(document.has_value(), "group_transform loads")) {
        return false;
    }
    bool ok = expect(document->shapes.size() == 1, "group_transform decodes one shape");
    if (!ok) {
        return false;
    }
    const auto& shape = document->shapes[0];
    ok = expect(is_rect_outline(shape.path, 12.0f, 8.0f, 24.0f, 16.0f),
                "translate+scale is baked into absolute geometry") &&
         expect(shape.paint.fill.has_value() && shape.paint.fill->r == 0x11 &&
                    shape.paint.fill->g == 0x22 && shape.paint.fill->b == 0x33,
                "group transform keeps the fill") &&
         ok;
    return ok;
}

bool viewbox_geometry() {
    const auto document = load_fixture("viewbox_scale.svg");
    if (!expect(document.has_value(), "viewbox_scale loads")) {
        return false;
    }
    bool ok = expect(document->width == 80 && document->height == 40,
                     "viewBox maps to the width/height document size") &&
              expect(document->shapes.size() == 1, "viewbox_scale decodes one shape");
    if (!ok) {
        return false;
    }
    ok = expect(is_rect_outline(document->shapes[0].path, 10.0f, 10.0f, 30.0f, 30.0f),
                "viewBox scale is baked into the geometry") &&
         ok;
    return ok;
}

bool stroke_and_opacity_geometry() {
    const auto document = load_fixture("stroke_path.svg");
    if (!expect(document.has_value(), "stroke_path loads")) {
        return false;
    }
    bool ok = expect(document->shapes.size() == 1, "stroke_path decodes one path");
    if (!ok) {
        return false;
    }
    const auto& shape = document->shapes[0];
    ok = expect(shape.path.segments.size() == 4 &&
                    is_segment(shape.path.segments[0], path::SegmentKind::move_to, 8.0f, 8.0f) &&
                    is_segment(shape.path.segments[1], path::SegmentKind::line_to, 56.0f, 8.0f) &&
                    is_segment(shape.path.segments[2], path::SegmentKind::line_to, 56.0f, 56.0f) &&
                    shape.path.segments[3].kind == path::SegmentKind::close,
                "path data maps to move/line/line/close") &&
         expect(shape.paint.fill.has_value() && shape.paint.fill->g == 0x8b &&
                    shape.paint.fill->a == 191,
                "fill-opacity 0.75 folds into the fill alpha") &&
         expect(!shape.paint.stroke.has_value(), "stroke-only fixture has no fill-less stroke") &&
         ok;
    return ok;
}

bool reject_unsupported() {
    const auto text = load_fixture("unsupported_text.svg");
    bool ok = expect(!text.has_value() && text.error() == path::SvgError::unsupported,
                     "text element is rejected as unsupported");

    constexpr std::string_view kGradient =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">"
        "<defs><linearGradient id=\"g\"><stop offset=\"0\"/></linearGradient></defs>"
        "<rect width=\"8\" height=\"8\" fill=\"url(#g)\"/></svg>";
    const auto gradient = path::load_svg(std::span<const char>(kGradient));
    ok = expect(!gradient.has_value() && gradient.error() == path::SvgError::unsupported,
                "gradient paint server is rejected as unsupported") &&
         ok;

    constexpr std::string_view kImage =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">"
        "<image href=\"pixels.png\" width=\"8\" height=\"8\"/></svg>";
    const auto image = path::load_svg(std::span<const char>(kImage));
    ok = expect(!image.has_value() && image.error() == path::SvgError::unsupported,
                "external image reference is rejected as unsupported") &&
         ok;

    constexpr std::string_view kDoctype =
        "<!DOCTYPE svg SYSTEM \"http://example.invalid/svg.dtd\">"
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\"/>";
    const auto doctype = path::load_svg(std::span<const char>(kDoctype));
    ok = expect(!doctype.has_value() && doctype.error() == path::SvgError::unsupported,
                "DOCTYPE/external entity declaration is rejected") &&
         ok;

    constexpr std::string_view kIgnored =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"8\" height=\"8\">"
        "<title>ignored</title><desc>ignored</desc><metadata>ignored</metadata>"
        "<rect width=\"4\" height=\"4\" fill=\"#010203\"/></svg>";
    const auto ignored = path::load_svg(std::span<const char>(kIgnored));
    ok = expect(ignored.has_value() && ignored->shapes.size() == 1,
                "title/desc/metadata are ignored, not rejected") &&
         ok;
    return ok;
}

bool reject_malformed() {
    const auto malformed = load_fixture("malformed.svg");
    bool ok = expect(!malformed.has_value() && malformed.error() == path::SvgError::malformed,
                     "truncated SVG reports malformed");
    const auto empty = path::load_svg(std::span<const char>());
    ok = expect(!empty.has_value() && empty.error() == path::SvgError::invalid_argument,
                "empty input reports invalid_argument") &&
         ok;
    return ok;
}

bool bounds() {
    const auto bytes = read_fixture("icon_shapes.svg");
    if (bytes.empty()) {
        return false;
    }
    path::SvgLimits too_small_input;
    too_small_input.max_input_bytes = 8;
    const auto big =
        path::load_svg(std::span<const std::uint8_t>(bytes.data(), bytes.size()), too_small_input);
    bool ok = expect(!big.has_value() && big.error() == path::SvgError::input_too_large,
                     "input larger than max_input_bytes reports input_too_large");
    if (!path::svg_backend_available()) {
        return ok;
    }

    path::SvgLimits one_shape;
    one_shape.max_shapes = 1;
    const auto shapes = load_fixture("icon_shapes.svg", one_shape);
    ok = expect(!shapes.has_value() && shapes.error() == path::SvgError::limit_exceeded,
                "shape budget reports limit_exceeded") &&
         ok;

    path::SvgLimits shallow;
    shallow.max_depth = 2;
    const auto depth = load_fixture("group_transform.svg", shallow);
    ok = expect(!depth.has_value() && depth.error() == path::SvgError::limit_exceeded,
                "depth budget reports limit_exceeded") &&
         ok;

    path::SvgLimits few_segments;
    few_segments.max_segments = 3;
    const auto segments = load_fixture("stroke_path.svg", few_segments);
    ok = expect(!segments.has_value() && segments.error() == path::SvgError::limit_exceeded,
                "segment budget reports limit_exceeded") &&
         ok;

    path::SvgLimits zero;
    zero.max_shapes = 0;
    const auto invalid = load_fixture("icon_shapes.svg", zero);
    ok = expect(!invalid.has_value() && invalid.error() == path::SvgError::invalid_argument,
                "nonsense limits report invalid_argument") &&
         ok;
    return ok;
}

[[nodiscard]] bool same_document(const path::SvgDocument& lhs, const path::SvgDocument& rhs) {
    if (lhs.width != rhs.width || lhs.height != rhs.height ||
        lhs.shapes.size() != rhs.shapes.size()) {
        return false;
    }
    for (std::size_t shape_index = 0; shape_index < lhs.shapes.size(); ++shape_index) {
        const auto& left = lhs.shapes[shape_index];
        const auto& right = rhs.shapes[shape_index];
        if (left.path.segments.size() != right.path.segments.size() ||
            left.path.fill_rule != right.path.fill_rule ||
            left.paint.fill.has_value() != right.paint.fill.has_value() ||
            left.paint.stroke.has_value() != right.paint.stroke.has_value()) {
            return false;
        }
        if (left.paint.fill.has_value() && right.paint.fill.has_value()) {
            const auto& a = *left.paint.fill;
            const auto& b = *right.paint.fill;
            if (a.r != b.r || a.g != b.g || a.b != b.b || a.a != b.a) {
                return false;
            }
        }
        if (left.paint.stroke.has_value() && right.paint.stroke.has_value()) {
            const auto& a = *left.paint.stroke;
            const auto& b = *right.paint.stroke;
            if (a.color.r != b.color.r || a.color.g != b.color.g || a.color.b != b.color.b ||
                a.color.a != b.color.a || a.width != b.width || a.cap != b.cap ||
                a.join != b.join || a.miter_limit != b.miter_limit) {
                return false;
            }
        }
        for (std::size_t segment_index = 0; segment_index < left.path.segments.size();
             ++segment_index) {
            const auto& a = left.path.segments[segment_index];
            const auto& b = right.path.segments[segment_index];
            if (a.kind != b.kind || a.p0.x != b.p0.x || a.p0.y != b.p0.y || a.p1.x != b.p1.x ||
                a.p1.y != b.p1.y || a.p2.x != b.p2.x || a.p2.y != b.p2.y) {
                return false;
            }
        }
    }
    return true;
}

bool determinism() {
    const auto first = load_fixture("icon_shapes.svg");
    const auto second = load_fixture("icon_shapes.svg");
    bool ok = expect(first.has_value() && second.has_value(), "two loads succeed") &&
              expect(first.has_value() && second.has_value() && same_document(*first, *second),
                     "two loads of the same bytes produce identical geometry");
    if (!path::svg_backend_available()) {
        return ok;
    }

    auto rasterizer = path::make_path_rasterizer(path::PathBackend::thorvg);
    if (!expect(rasterizer != nullptr, "ThorVG rasterizer is available")) {
        return false;
    }
    const auto stroke = load_fixture("stroke_path.svg");
    if (!expect(stroke.has_value() && stroke->shapes.size() == 1, "stroke_path loads one shape")) {
        return false;
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kIconSize) * kIconSize * 4U, 0);
    path::RasterTarget target{std::span<std::uint8_t>(pixels), kIconSize, kIconSize,
                              kIconSize * 4U};
    const auto raster =
        rasterizer->rasterize(std::span<const path::PathShape>(&stroke->shapes[0], 1), target);
    const std::uint64_t hash_1x = fnv1a(pixels);
    std::cout << "svg_loader: stroke_path_fnv_1x=0x" << std::hex << hash_1x << std::dec << "\n";
    ok = expect(raster.has_value() && hash_1x == kStrokePathFnv1x,
                "1x rasterization matches the checked-in cross-process FNV") &&
         ok;

    FakeDevice device;
    path::PathTextureCache cache(device, *rasterizer, {});
    const auto key =
        path::make_path_texture_key(1, 1, stroke->width, stroke->height, 2000, stroke->shapes[0]);
    const auto uploaded = cache.acquire(key, stroke->shapes[0]);
    if (!expect(uploaded.has_value() && uploaded->extent.width == 128 &&
                    uploaded->extent.height == 128,
                "2x upload is a 128x128 texture")) {
        return false;
    }
    const auto* texture = fake_texture(*uploaded);
    if (!expect(texture != nullptr, "cache uploaded readable bytes")) {
        return false;
    }
    const std::uint64_t hash_2x = fnv1a(texture->bytes());
    std::cout << "svg_loader: stroke_path_fnv_2x=0x" << std::hex << hash_2x << std::dec << "\n";
    ok = expect(hash_2x == kStrokePathFnv2x,
                "2x cache upload matches the checked-in cross-process FNV") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = backend_behavior() && ok;
    if (path::svg_backend_available()) {
        ok = icon_shapes_geometry() && ok;
        ok = group_transform_geometry() && ok;
        ok = viewbox_geometry() && ok;
        ok = stroke_and_opacity_geometry() && ok;
        ok = reject_unsupported() && ok;
        ok = reject_malformed() && ok;
        ok = bounds() && ok;
        ok = determinism() && ok;
    } else {
        std::cout << "svg_loader: ThorVG OFF, geometry checks skipped (backend_unavailable)";
        std::cout << "\n";
    }
    if (!ok) {
        std::cerr << "svg_loader: FAILED\n";
        return 1;
    }
    std::cout << "svg_loader: ok\n";
    return 0;
}
