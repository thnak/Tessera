// SPDX-License-Identifier: MIT
//
// Vulkan-free CPU paint goldens for four V1 paint capabilities: borders, nested clips, linear
// gradients and the carved outer box shadow (#461). Each case is an ordinary scene graph painted by
// the shared paint pass
// (`ui::paint::paint_scene`) into `CpuPaintSink` (`tools/poc/batched2d/cpu_paint_sink.hpp`), which
// maps the commands onto the deterministic `CpuCanvas`: axis-aligned fills, an intersecting clip
// stack, the documented rounded-bounds approximation, and 64-band gradients sampled through
// `ui::paint::sample_linear_gradient`. `ui::paint::Batched2DSink` is the GPU twin of that sink (see
// `paint_gradient_tests.cpp`); it needs a device, so the committed CPU baselines use the CPU sink.
//
// The four baselines under `tests/golden/baseline/cpu_wide/` are generated on the CPU path only,
// through this executable's own write mode:
//
//     cpu_wide_golden_tests --update-goldens
//
// They are compared bit-exactly (tolerance 0, zero mismatched pixels), so they are bit-exact on
// Windows and Linux: no GPU, lavapipe, wall clock, random id or system font is involved and no text
// is painted. Regenerating them must not change a byte.

// Shared scaffolding (canvas, sink, stub style provider, pixel helpers, scene colours) lives in
// `cpu_wide_scene_support.hpp`, so each case below reads as a scene.
#include "cpu_wide_scene_support.hpp"
#include "tessera/rendering/golden/image_diff.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

#ifndef TESSERA_CPU_WIDE_GOLDEN_DIR
#error "TESSERA_CPU_WIDE_GOLDEN_DIR must point at tests/golden/baseline/cpu_wide"
#endif

namespace {

using namespace tessera::poc::batched2d::cpu_wide_support;

using tessera::poc::batched2d::CpuPaintSink;
using tessera::rendering::golden::ImageSize;

// `--update-goldens` rewrites the committed baselines from the CPU path only; ctest never passes
// it.
bool update_goldens = false;

// -------------------------------------------------------------------------------------------------
// borders: a rounded card with four distinct border sides, a square card with a uniform border, and
// a borderless rounded card that pins the documented bounds approximation for corners.
// -------------------------------------------------------------------------------------------------
void build_borders_scene(Node& root, StubStyles& styles) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1120"}}));

    Node& card = add_node(root, 2, {24, 24, 160, 110});
    styles.set(2, styled({{"background-color", "#1E293B"},
                          {"border-top-width", "4px"},
                          {"border-top-style", "solid"},
                          {"border-top-color", "#38BDF8"},
                          {"border-right-width", "6px"},
                          {"border-right-style", "solid"},
                          {"border-right-color", "#F472B6"},
                          {"border-bottom-width", "8px"},
                          {"border-bottom-style", "solid"},
                          {"border-bottom-color", "#FACC15"},
                          {"border-left-width", "2px"},
                          {"border-left-style", "solid"},
                          {"border-left-color", "#22C55E"},
                          {"border-top-left-radius", "12px"},
                          {"border-top-right-radius", "12px"},
                          {"border-bottom-right-radius", "12px"},
                          {"border-bottom-left-radius", "12px"}}));
    add_node(card, 3, {16, 20, 60, 30});
    styles.set(3, styled({{"background-color", "#E2E8F0"}}));

    add_node(root, 4, {208, 48, 88, 120});
    styles.set(4, styled({{"background-color", "#111827"},
                          {"border-top-width", "6px"},
                          {"border-top-style", "solid"},
                          {"border-top-color", "#A78BFA"},
                          {"border-right-width", "6px"},
                          {"border-right-style", "solid"},
                          {"border-right-color", "#A78BFA"},
                          {"border-bottom-width", "6px"},
                          {"border-bottom-style", "solid"},
                          {"border-bottom-color", "#A78BFA"},
                          {"border-left-width", "6px"},
                          {"border-left-style", "solid"},
                          {"border-left-color", "#A78BFA"}}));

    add_node(root, 5, {24, 152, 60, 36});
    styles.set(5, styled({{"background-color", "#38BDF8"},
                          {"border-top-left-radius", "10px"},
                          {"border-top-right-radius", "10px"},
                          {"border-bottom-right-radius", "10px"},
                          {"border-bottom-left-radius", "10px"}}));
}

bool check_borders(const PaintStats& stats, const CpuCanvas& canvas) {
    bool ok = expect(stats.nodes == 5, "borders: five nodes painted") &&
              expect(stats.borders == 8, "borders: both bordered cards emit all four edges") &&
              expect(stats.backgrounds == 5, "borders: every node paints one background");
    ok = expect(is_pixel(canvas, 24, 24, kAccent), "borders: top edge") && ok;
    ok = expect(is_pixel(canvas, 183, 60, kBorderRight), "borders: right edge") && ok;
    ok = expect(is_pixel(canvas, 100, 133, kBorderBottom), "borders: bottom edge") && ok;
    ok = expect(is_pixel(canvas, 24, 60, kBorderLeft), "borders: left edge") && ok;
    ok = expect(is_pixel(canvas, 40, 44, kInner), "borders: child paints over the parent") && ok;
    ok = expect(is_pixel(canvas, 207, 50, kRoot), "borders: nothing paints outside the card") && ok;
    ok =
        expect(is_pixel(canvas, 208, 48, kBorderUniform), "borders: uniform square top edge") && ok;
    ok = expect(is_pixel(canvas, 250, 90, kCardAlt), "borders: uniform card interior") && ok;
    ok = expect(is_pixel(canvas, 295, 90, kBorderUniform), "borders: uniform right edge") && ok;
    ok = expect(is_pixel(canvas, 208, 90, kBorderUniform), "borders: uniform left edge") && ok;
    // The CPU sink paints a rounded rect as its axis-aligned bounds, so the corner outside the arc
    // is filled. Pinning it keeps the documented approximation visible.
    ok = expect(is_pixel(canvas, 24, 152, kAccent),
                "borders: rounded corner falls back to the bounds (documented CPU gap)") &&
         ok;
    return ok;
}

// -------------------------------------------------------------------------------------------------
// clips: three nested overflow clips cut a chain of fills, the outermost clip starts off-canvas,
// and a disjoint clip must drop its child entirely.
// -------------------------------------------------------------------------------------------------
void build_clips_scene(Node& root, StubStyles& styles) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1120"}}));

    // Absolute box x -16..134, y 24..144: the clip starts off-canvas, so the panel is cut on the
    // left/top of the canvas while its own background still paints inside the visible part.
    Node& outer = add_node(root, 2, {-16, 24, 150, 120});
    styles.set(2, styled({{"background-color", "#1E293B"}}));
    clip_children(styles, 2);

    Node& middle = add_node(outer, 3, {36, 16, 80, 96});
    styles.set(3, styled({{"background-color", "#334155"}}));
    clip_children(styles, 3);

    Node& leaf = add_node(middle, 4, {28, 28, 120, 80});
    styles.set(4, styled({{"background-color", "#EF4444"}}));
    clip_children(styles, 4);

    add_node(leaf, 5, {0, 0, 200, 40});
    styles.set(5, styled({{"background-color", "#F97316"}}));

    // Disjoint clip: the panel clips x 220..280 and the child sits at x 290..330, so even the part
    // of the child that is on the canvas must paint nothing.
    Node& panel = add_node(root, 6, {220, 40, 60, 50});
    styles.set(6, styled({{"background-color", "#475569"}}));
    clip_children(styles, 6);
    add_node(panel, 7, {70, 10, 40, 30});
    styles.set(7, styled({{"background-color", "#22C55E"}}));
}

bool check_clips(const PaintStats& stats, const CpuCanvas& canvas) {
    bool ok = expect(stats.nodes == 7, "clips: seven nodes painted") &&
              expect(stats.clips == 4, "clips: three nested clips plus the disjoint clip") &&
              expect(stats.hidden_subtrees == 0, "clips: no hidden subtree");
    ok = expect(is_pixel(canvas, 60, 40, kMiddle), "clips: the middle panel paints its own box") &&
         ok;
    ok = expect(is_pixel(canvas, 60, 80, kOrange),
                "clips: the grandchild shows inside the three-way intersection") &&
         ok;
    ok = expect(is_pixel(canvas, 60, 96, kOrange),
                "clips: the grandchild is cut at the leaf edge") &&
         ok;
    ok = expect(is_pixel(canvas, 60, 120, kLeaf),
                "clips: the leaf background stops at the middle clip") &&
         ok;
    ok = expect(is_pixel(canvas, 60, 140, kCard),
                "clips: below the middle clip only the outer panel shows") &&
         ok;
    ok = expect(is_pixel(canvas, 110, 80, kCard),
                "clips: outside the middle clip only the outer panel shows") &&
         ok;
    ok = expect(is_pixel(canvas, 140, 80, kRoot), "clips: outside the outer clip the root shows") &&
         ok;
    ok = expect(is_pixel(canvas, 0, 30, kCard),
                "clips: the off-canvas outer clip still paints its on-canvas part") &&
         ok;
    ok = expect(is_pixel(canvas, 230, 50, kSlate), "clips: the disjoint panel paints itself") && ok;
    ok = expect(is_pixel(canvas, 300, 60, kRoot),
                "clips: a disjoint clip drops every child pixel, including on-canvas ones") &&
         ok;
    return ok;
}

// -------------------------------------------------------------------------------------------------
// gradients: a vertical and a horizontal linear ramp (two stops each, the vertical one with a
// middle stop) and a third gradient cut by two nested overflow clips.
// -------------------------------------------------------------------------------------------------
void build_gradients_scene(Node& root, StubStyles& styles) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1120"}}));

    Node& vertical = add_node(root, 2, {16, 16, 120, 160});
    vertical.set_paint_data(
        gradient_payload(180.0f, {LinearGradientStop{0.0f, rgba8(29, 78, 216)},
                                  LinearGradientStop{0.5f, rgba8(124, 58, 237)},
                                  LinearGradientStop{1.0f, rgba8(219, 39, 119)}}));

    Node& horizontal = add_node(root, 3, {152, 16, 148, 70});
    horizontal.set_paint_data(
        gradient_payload(90.0f, {LinearGradientStop{0.0f, rgba8(16, 185, 129)},
                                 LinearGradientStop{1.0f, rgba8(245, 158, 11)}}));

    Node& outer = add_node(root, 4, {152, 100, 148, 76});
    styles.set(4, styled({{"background-color", "#1E293B"}}));
    clip_children(styles, 4);
    Node& inner = add_node(outer, 5, {8, 8, 120, 56});
    styles.set(5, styled({{"background-color", "#334155"}}));
    clip_children(styles, 5);
    Node& clipped = add_node(inner, 6, {-10, -10, 160, 100});
    clipped.set_paint_data(
        gradient_payload(180.0f, {LinearGradientStop{0.0f, rgba8(14, 165, 233)},
                                  LinearGradientStop{1.0f, rgba8(244, 63, 94)}}));
}

bool check_gradients(const PaintStats& stats, const CpuCanvas& canvas) {
    bool ok = expect(stats.nodes == 6, "gradients: six nodes painted") &&
              expect(stats.gradients == 3, "gradients: three linear fills emitted");
    ok = expect(channel(canvas, 76, 17, 2) > channel(canvas, 76, 17, 0),
                "gradients: the vertical ramp starts blue") &&
         ok;
    ok = expect(channel(canvas, 76, 174, 0) > channel(canvas, 76, 174, 2),
                "gradients: the vertical ramp ends red") &&
         ok;
    ok = expect(channel(canvas, 160, 50, 1) > channel(canvas, 160, 50, 0),
                "gradients: the horizontal ramp starts green") &&
         ok;
    ok = expect(channel(canvas, 295, 50, 0) > channel(canvas, 295, 50, 2),
                "gradients: the horizontal ramp ends amber") &&
         ok;
    ok = expect(channel(canvas, 170, 110, 2) > channel(canvas, 170, 110, 0),
                "gradients: the clipped ramp still paints inside both clips") &&
         ok;
    ok = expect(is_pixel(canvas, 285, 120, kCard),
                "gradients: outside the inner clip the outer panel shows") &&
         ok;
    ok = expect(is_pixel(canvas, 200, 170, kCard),
                "gradients: the nested clip cuts the gradient's bottom") &&
         ok;
    ok =
        expect(is_pixel(canvas, 310, 190, kRoot), "gradients: nothing paints outside the panels") &&
        ok;
    return ok;
}

// -------------------------------------------------------------------------------------------------
// shadows (issue #461): a transparent square box, a transparent rounded box and an opaque card, all
// with the same blurred shadow. CSS paints an outer shadow only outside the border box, so the two
// transparent boxes keep the page colour inside - corner triangles of the rounded one included, the
// CPU sink's bounds approximation of rounding - while the shadow shows around and below them. The
// opaque card pins the unchanged case: its background covers everything the carve removed.
// -------------------------------------------------------------------------------------------------
constexpr Pixel kShadowInside = kRoot;

void build_shadows_scene(Node& root, StubStyles& styles) {
    root.set_bounds({0, 0, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    styles.set(1, styled({{"background-color", "#0B1120"}}));

    add_node(root, 2, {24, 24, 96, 64});
    styles.set(2, styled({{"box-shadow", "0px 2px 8px rgba(56, 189, 248, 0.55)"}}));

    add_node(root, 3, {152, 24, 96, 64});
    styles.set(3, styled({{"box-shadow", "0px 2px 8px rgba(56, 189, 248, 0.55)"},
                          {"border-top-left-radius", "16px"},
                          {"border-top-right-radius", "16px"},
                          {"border-bottom-right-radius", "16px"},
                          {"border-bottom-left-radius", "16px"}}));

    add_node(root, 4, {24, 124, 96, 64});
    styles.set(4, styled({{"background-color", "#1E293B"},
                          {"box-shadow", "0px 2px 8px rgba(56, 189, 248, 0.55)"}}));
}

bool check_shadows(const PaintStats& stats, const CpuCanvas& canvas) {
    bool ok =
        expect(stats.nodes == 4, "shadows: four nodes painted") &&
        expect(stats.shadows == 12, "shadows: four blur layers for each of the three shadows");
    ok =
        expect(is_pixel(canvas, 72, 56, kShadowInside) && is_pixel(canvas, 26, 26, kShadowInside) &&
                   is_pixel(canvas, 118, 86, kShadowInside),
               "shadows: a transparent square box keeps the page colour inside") &&
        ok;
    ok = expect(channel(canvas, 72, 92, 2) > channel(canvas, 72, 92, 0),
                "shadows: the blurred shadow still paints below the transparent box") &&
         ok;
    ok = expect(is_pixel(canvas, 200, 56, kShadowInside) &&
                    is_pixel(canvas, 154, 26, kShadowInside) &&
                    is_pixel(canvas, 246, 86, kShadowInside),
                "shadows: a transparent rounded box keeps it inside, corners included") &&
         ok;
    ok = expect(channel(canvas, 200, 92, 2) > channel(canvas, 200, 92, 0),
                "shadows: the rounded box's shadow paints below it") &&
         ok;
    ok = expect(is_pixel(canvas, 72, 156, kCard),
                "shadows: the opaque card is unchanged over its own shadow") &&
         ok;
    ok = expect(channel(canvas, 72, 192, 2) > channel(canvas, 72, 192, 0),
                "shadows: the opaque card's shadow paints below it") &&
         ok;
    ok = expect(is_pixel(canvas, 300, 10, kRoot),
                "shadows: nothing paints outside the three boxes") &&
         ok;
    return ok;
}

using BuildFn = void (*)(Node&, StubStyles&);
using CheckFn = bool (*)(const PaintStats&, const CpuCanvas&);

bool run_case(const char* name, BuildFn build, CheckFn check) {
    Node root(1);
    StubStyles styles;
    build(root, styles);

    CpuCanvas canvas = make_canvas();
    CpuPaintSink sink(canvas);
    const PaintStats stats = tessera::ui::paint::paint_scene(root, styles, sink);
    const std::vector<std::uint8_t> first = bytes(canvas);

    // Deterministic repeat before touching the baseline, so a flaky render can never look like a
    // stale golden.
    CpuCanvas repeat_canvas = make_canvas();
    CpuPaintSink repeat_sink(repeat_canvas);
    tessera::ui::paint::paint_scene(root, styles, repeat_sink);
    const std::vector<std::uint8_t> repeat = bytes(repeat_canvas);
    bool ok = expect(first == repeat, std::string(name) + ": two renders are byte-identical") &&
              check(stats, canvas);

    const std::filesystem::path baseline =
        std::filesystem::path(TESSERA_CPU_WIDE_GOLDEN_DIR) / (std::string(name) + ".png");
    if (update_goldens) {
        std::error_code error;
        std::filesystem::create_directories(baseline.parent_path(), error);
        const int wrote = stbi_write_png(baseline.string().c_str(), static_cast<int>(kWidth),
                                         static_cast<int>(kHeight), 4, first.data(),
                                         static_cast<int>(kWidth) * 4);
        ok = expect(wrote != 0, std::string(name) + ": baseline written") && ok;
        std::cout << "cpu-wide-golden: wrote " << baseline.string() << "\n";
        return ok;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded =
        stbi_load(baseline.string().c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (decoded == nullptr) {
        return expect(false, std::string(name) + ": cannot read baseline " + baseline.string());
    }
    const std::vector<std::uint8_t> expected(
        decoded, decoded + static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    stbi_image_free(decoded);

    ok = expect(width == static_cast<int>(kWidth) && height == static_cast<int>(kHeight),
                std::string(name) + ": baseline dimensions match the scene") &&
         ok;
    if (width == static_cast<int>(kWidth) && height == static_cast<int>(kHeight)) {
        const auto diff = tessera::rendering::golden::compare_rgba(
            ImageSize{kWidth, kHeight}, first,
            ImageSize{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)},
            expected, 0);
        if (diff.mismatched != 0) {
            std::cerr << "cpu-wide-golden: " << name << " baseline mismatch (" << diff.mismatched
                      << "/" << (kWidth * kHeight) << " pixels)\n";
        }
        ok = expect(diff.mismatched == 0 && !diff.size_mismatch,
                    std::string(name) + ": matches the baseline bit-exactly") &&
             ok;
    }
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::string(argv[index]) == "--update-goldens") {
            update_goldens = true;
            continue;
        }
        std::cerr << "cpu-wide-golden: unknown argument " << argv[index] << "\n";
        return 2;
    }

    bool ok = run_case("borders", build_borders_scene, check_borders);
    ok = run_case("clips", build_clips_scene, check_clips) && ok;
    ok = run_case("gradients", build_gradients_scene, check_gradients) && ok;
    ok = run_case("shadows", build_shadows_scene, check_shadows) && ok;
    if (!ok) {
        std::cerr << "cpu-wide-golden: FAILED\n";
        return 1;
    }
    std::cout << "cpu-wide-golden: ok\n";
    return 0;
}
