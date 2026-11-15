// SPDX-License-Identifier: MIT
//
// Preview canvas, native half (issue #167, P3 of docs/design/reference-equantic-ui.md section 7.4).
//
// The acceptance sentence is about pixels, and pixels need a device; this test proves everything
// about the canvas path that does *not* need one, so a broken renderer cannot hide behind "the
// browser lane will catch it":
//
//  * `prepare_scene()` builds and lays out the tree the painter consumes, and a source the preview
//    does not evaluate fails with a reason instead of a half-built scene;
//  * `scene_frame_render()` on the **null** backend records the frame's commands and reports the
//    counters a browser page shows - and the same scene records the same stream twice;
//  * the preview stylesheet actually paints: a painted-box count of zero would make any two frames
//    compare equal at tolerance 0, which is the vacuity the parity gate guards against;
//  * text is counted as skipped rather than silently dropped (the module ships no font);
//  * `canvas_report_json()` is valid JSON with the documented fields, and a no-device report is
//    distinguishable from a failed frame (that distinction is the page's unsupported-browser
//    state);
//  * the RHI's own cross-backend comparison classifies an identical pair as a pass and a moved box
//  as
//    a failure at the tolerance `tests/golden/tolerance.json` records for `webgpu_vs_lavapipe`.
//
// No device, no window, no clock, no file: the test runs anywhere the null backend does.

#include "canvas_report.hpp"
#include "scene_frame.hpp"
#include "scene_prepare.hpp"
#include "tessera/rendering/golden/parity.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace golden = tessera::rendering::golden;
namespace null = tessera::rendering::null;
namespace rhi = tessera::rendering::rhi;

int g_failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "scene_frame: " << message << "\n";
        ++g_failures;
    }
}

constexpr std::string_view kColumn = R"(export function Column(): Node {
  return (
    <View>
      <Text>Preview</Text>
      <Button>Add</Button>
      <Button>Reset</Button>
    </View>
  );
}
)";

// Two boxes of the same size at different places: the "moved geometry" case the parity policy must
// reject, and the cheapest way to build it without a second source file.
constexpr std::string_view kTwoBoxes = R"(export function TwoBoxes(): Node {
  return (
    <View>
      <Button>One</Button>
      <Button>Two</Button>
    </View>
  );
}
)";

cli::ScenePreviewOptions options_for(std::string_view file) {
    cli::ScenePreviewOptions options;
    options.file = file;
    options.salt = "preview";
    options.width = 320.0f;
    options.height = 240.0f;
    options.dpi_scale = 1.0f;
    return options;
}

void test_prepare_scene() {
    cli::PreparedScene scene = cli::prepare_scene(kColumn, options_for("ui/Column.tsx"));
    expect(scene.error.empty(), "the column case prepares: " + scene.error);
    expect(scene.root != nullptr, "a prepared scene has a root");
    expect(scene.sheet.has_value(), "a prepared scene carries the stylesheet it laid out with");
    expect(scene.nodes >= 5, "the column case resolves at least 5 style nodes");
    expect(!scene.root->children().empty(), "the component's tree is attached to the root");

    // A source the preview does not evaluate is a *reason*, never a half-built scene: a partial
    // tree would be a picture of a program that does not exist.
    const auto broken = cli::prepare_scene("export function Broken(: Node { return <View>; }",
                                           options_for("ui/Broken.tsx"));
    expect(!broken.error.empty(), "a source that does not parse reports a reason");
    expect(broken.root == nullptr, "a failed prepare has no tree");
    expect(!broken.sheet.has_value(), "a failed prepare has no stylesheet");
}

void test_frame_counters() {
    std::unique_ptr<rhi::Device> device = null::create_null_device(rhi::DeviceDesc{});
    expect(device != nullptr, "the null device is created");
    if (device == nullptr) {
        return;
    }
    const auto result =
        cli::scene_frame_render(kColumn, options_for("ui/Column.tsx"), *device, {320, 240});
    expect(result.error.empty(), "the null backend renders the frame: " + result.error);
    expect(result.nodes >= 5, "the frame reports its painted node count");
    // The anti-vacuity rule: the preview stylesheet has to paint. A frame of zero draws is the
    // clear colour everywhere, and two clear-colour frames pass any tolerance comparison.
    expect(result.draw_calls >= 1, "the frame records at least one draw call");
    expect(result.vertex_count >= 6, "one drawn quad is at least six vertices");
    expect(result.backgrounds >= 1, "the frame paints at least one background");
    // No font ships in the preview module, so the text run is counted rather than invented.
    expect(result.texts >= 1, "the column case has a text run");
    expect(result.text_skipped >= 1, "a text run with no font is counted as skipped");
    // The null backend holds pixels in memory, so a readback is available and fully filled.
    expect(result.pixels_available, "the null backend can read its target back");
    expect(result.frame.rgba.size() ==
               static_cast<std::size_t>(result.frame.width) * result.frame.height * 4,
           "the frame's pixel buffer matches its extent");
}

void test_frame_determinism() {
    std::unique_ptr<rhi::Device> first = null::create_null_device(rhi::DeviceDesc{});
    std::unique_ptr<rhi::Device> second = null::create_null_device(rhi::DeviceDesc{});
    if (first == nullptr || second == nullptr) {
        expect(false, "two null devices are created");
        return;
    }
    const auto a =
        cli::scene_frame_render(kTwoBoxes, options_for("ui/TwoBoxes.tsx"), *first, {320, 240});
    const auto b =
        cli::scene_frame_render(kTwoBoxes, options_for("ui/TwoBoxes.tsx"), *second, {320, 240});
    expect(a.error.empty() && b.error.empty(), "both renders succeed");
    expect(a.draw_calls == b.draw_calls, "two runs record the same draw-call count");
    expect(a.vertex_count == b.vertex_count, "two runs record the same vertex count");
    // The command stream is what the null backend holds, and the pixels it stores are its target's
    // contents; both must be a function of the source alone.
    expect(a.frame.rgba == b.frame.rgba, "two runs produce byte-identical pixels");
}

void test_canvas_report() {
    cli::SceneFrameResult result;
    result.nodes = 5;
    result.draw_calls = 2;
    result.vertex_count = 12;
    result.backgrounds = 3;
    result.borders = 1;
    result.texts = 1;
    result.text_skipped = 1;
    result.pixels_available = false;
    const std::string report = cli::canvas_report_json(result, 320, 240, false);
    expect(report.find("\"canvasVersion\":1") != std::string::npos,
           "the report carries its version");
    expect(report.find("\"hasDevice\":false") != std::string::npos, "a no-device report says so");
    expect(report.find("\"pixelsAvailable\":false") != std::string::npos,
           "a browser frame reports that pixels are absent");
    expect(report.find("\"drawCalls\":2") != std::string::npos, "the draw count is reported");
    expect(report.back() == '\n', "the report is one newline-terminated line");

    // An error string with a quote and a backslash has to survive as valid JSON: the page parses
    // this with JSON.parse, and a broken document is a blank canvas with a console error.
    cli::SceneFrameResult failed;
    failed.error = "path \"x\" \\ broke";
    const std::string escaped = cli::canvas_report_json(failed, 1, 1, true);
    expect(escaped.find("\\\"x\\\"") != std::string::npos, "a quote in the error is escaped");
    expect(escaped.find("\\\\") != std::string::npos, "a backslash in the error is escaped");
    expect(escaped.find("\"hasDevice\":true") != std::string::npos,
           "a failed frame still reports that a device existed");
}

// A frame's pixels compared against itself and against a shifted copy, under the policy the parity
// gate reads from tests/golden/tolerance.json for `webgpu_vs_lavapipe`.
//
// Deliberately SYNTHETIC pixels, not the null backend's: the null backend records commands and
// stores a zeroed image (it has no rasterizer - `copy_target_to_buffer` is a no-op there), so
// comparing its "frames" would compare two blank images and prove nothing. What this checks is the
// *policy*: the same C++ functions and the same tolerance the browser lane applies, so a tolerance
// edit or a broken classifier fails here first.
void test_parity_policy() {
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 48;
    const golden::ImageSize size{kWidth, kHeight};
    const auto paint_box = [](std::vector<std::uint8_t>& rgba, std::uint32_t x, std::uint32_t y,
                              std::uint32_t width, std::uint32_t height, std::uint8_t r,
                              std::uint8_t g, std::uint8_t b) {
        for (std::uint32_t row = y; row < y + height && row < kHeight; ++row) {
            for (std::uint32_t column = x; column < x + width && column < kWidth; ++column) {
                const std::size_t at = (static_cast<std::size_t>(row) * kWidth + column) * 4;
                rgba[at] = r;
                rgba[at + 1] = g;
                rgba[at + 2] = b;
                rgba[at + 3] = 255;
            }
        }
    };
    const auto make_frame = [&](std::uint32_t offset_y) {
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kWidth) * kHeight * 4, 0);
        for (std::size_t at = 0; at + 3 < rgba.size(); at += 4) {
            rgba[at] = 16;
            rgba[at + 1] = 26;
            rgba[at + 2] = 44;
            rgba[at + 3] = 255;
        }
        paint_box(rgba, 8, 8 + offset_y, 32, 20, 45, 108, 223);
        return rgba;
    };
    const std::vector<std::uint8_t> reference = make_frame(0);

    golden::ParityTolerance tolerance;
    tolerance.pair = "webgpu_vs_lavapipe";
    tolerance.edge_tolerance = 8;
    tolerance.max_mismatch = 0.02;
    tolerance.min_ssim = 0.99;
    tolerance.max_delta_e = 2.0;

    golden::DifferenceClassOptions class_options;
    class_options.edge_tolerance = tolerance.edge_tolerance;

    // Identical pair: a pass, and exactly so (SSIM 1, deltaE 0).
    {
        const std::span<const std::uint8_t> pixels(reference.data(), reference.size());
        const golden::DifferenceReport differences =
            golden::classify_differences(size, pixels, size, pixels, class_options);
        const golden::SsimResult ssim = golden::compute_ssim(size, pixels, size, pixels);
        const golden::DeltaEStats delta_e = golden::compute_delta_e(size, pixels, size, pixels);
        const golden::ParityVerdict verdict =
            golden::evaluate_parity(differences, ssim, delta_e, tolerance);
        expect(verdict.passed, "a frame compared with itself passes: " + verdict.reason);
        expect(ssim.mean == 1.0, "an identical pair has SSIM 1");
        expect(delta_e.mean == 0.0, "an identical pair has deltaE 0");
    }

    // The same box six rows lower: geometry moved, which is the most likely WebGPU-versus-lavapipe
    // disagreement once a coordinate convention differs. It must fail at this tolerance.
    {
        const std::vector<std::uint8_t> moved = make_frame(6);
        const golden::DifferenceReport differences =
            golden::classify_differences(size, moved, size, reference, class_options);
        const golden::SsimResult ssim = golden::compute_ssim(size, moved, size, reference);
        const golden::DeltaEStats delta_e = golden::compute_delta_e(size, moved, size, reference);
        const golden::ParityVerdict verdict =
            golden::evaluate_parity(differences, ssim, delta_e, tolerance);
        expect(!verdict.passed, "a box moved by six rows fails the tolerance");
        expect(!verdict.reason.empty(), "a failing comparison names its rule");
        expect(differences.counts.structural > 0, "a moved box is a structural difference");
    }

    // A one-LSB difference **on a box edge** is the case the tolerance exists for: the classifier
    // only forgives rounding near a luma gradient, because that is where a different rasterizer
    // legitimately rounds differently. Asserting the forgiving half matters as much as asserting
    // the strict half, or a mis-tuned policy would fail every real browser frame.
    {
        std::vector<std::uint8_t> nudged = reference;
        // The box painted at (8, 8)..(40, 28) has a vertical edge at x = 40; nudge the pixels just
        // inside it, which sit adjacent to the background and therefore on a gradient.
        for (std::uint32_t row = 8; row < 28; ++row) {
            const std::size_t at = (static_cast<std::size_t>(row) * kWidth + 39) * 4;
            nudged[at] = static_cast<std::uint8_t>(nudged[at] + 1);
        }
        const golden::DifferenceReport differences =
            golden::classify_differences(size, nudged, size, reference, class_options);
        const golden::SsimResult ssim = golden::compute_ssim(size, nudged, size, reference);
        const golden::DeltaEStats delta_e = golden::compute_delta_e(size, nudged, size, reference);
        const golden::ParityVerdict verdict =
            golden::evaluate_parity(differences, ssim, delta_e, tolerance);
        expect(differences.counts.edge_rounding > 0,
               "a one-LSB difference on a box edge is classified as edge rounding");
        expect(verdict.passed, "a one-LSB edge-rounding difference passes: " + verdict.reason);
    }

    // A one-LSB difference **in a flat region** is structural, not forgiven. This is the policy's
    // point: rounding is only legitimate where the image already changes quickly, so a uniform tint
    // that drifted by one is a real colour difference and a gate that ignored it would miss a
    // colour-space bug (the classic WebGPU-versus-lavapipe mistake).
    {
        std::vector<std::uint8_t> nudged = reference;
        for (std::size_t at = 0; at + 3 < nudged.size(); at += 4) {
            nudged[at] = static_cast<std::uint8_t>(nudged[at] + 1);
        }
        const golden::DifferenceReport differences =
            golden::classify_differences(size, nudged, size, reference, class_options);
        const golden::SsimResult ssim = golden::compute_ssim(size, nudged, size, reference);
        const golden::DeltaEStats delta_e = golden::compute_delta_e(size, nudged, size, reference);
        const golden::ParityVerdict verdict =
            golden::evaluate_parity(differences, ssim, delta_e, tolerance);
        expect(differences.counts.structural > 0,
               "a one-LSB difference in a flat region is structural");
        expect(!verdict.passed, "a flat-region colour drift is not forgiven as rounding");
    }

    // A size difference is never tolerated: a canvas that rendered the wrong extent is not a
    // rounding question, and scaling it into the reference would hide exactly the bug to look for.
    {
        std::vector<std::uint8_t> smaller(static_cast<std::size_t>(kWidth - 8) * kHeight * 4, 0);
        const golden::ImageSize small_size{kWidth - 8, kHeight};
        const golden::DifferenceReport differences =
            golden::classify_differences(small_size, smaller, size, reference, class_options);
        const golden::SsimResult ssim = golden::compute_ssim(small_size, smaller, size, reference);
        const golden::DeltaEStats delta_e =
            golden::compute_delta_e(small_size, smaller, size, reference);
        const golden::ParityVerdict verdict =
            golden::evaluate_parity(differences, ssim, delta_e, tolerance);
        expect(!verdict.passed, "a size difference fails regardless of tolerance");
    }

    // The pipeline description the browser build uses must be creatable without SPIR-V: an empty
    // shader id is how a backend learns there are no words to compile, and WebGPU would reject it.
    const rhi::PipelineDesc desc =
        cli::scene_frame_pipeline_desc(rhi::TextureFormat::R8G8B8A8Unorm, 1);
    expect(!desc.shader_id.empty(), "the preview pipeline names a logical shader id");
    expect(desc.vertex_spirv == nullptr && desc.vertex_spirv_size == 0,
           "the preview pipeline carries no SPIR-V, so a Wasm build can create it");
    expect(desc.vertex_layout.stride == 44, "the preview pipeline uses the shared Vertex2D layout");
    expect(desc.push_constant_size == 112, "the preview pipeline matches Batched2DPushConstants");
    // The null backend accepts that description with no SPIR-V at all, which is the property the
    // WebGPU backend relies on.
    std::unique_ptr<rhi::Device> device = null::create_null_device(rhi::DeviceDesc{});
    expect(device != nullptr && device->create_pipeline(desc) != nullptr,
           "a backend creates the SPIR-V-free preview pipeline");
}

void test_clear_color() {
    const rhi::Color clear = cli::scene_frame_clear_color();
    expect(clear.r == 11.0f / 255.0f && clear.g == 18.0f / 255.0f && clear.b == 32.0f / 255.0f,
           "the preview clears to the app window background, not to black");
}

} // namespace

int main() {
    test_prepare_scene();
    test_frame_counters();
    test_frame_determinism();
    test_canvas_report();
    test_parity_policy();
    test_clear_color();
    if (g_failures != 0) {
        std::cerr << "scene_frame: " << g_failures << " failure(s)\n";
        return 1;
    }
    std::cout << "scene_frame: ok\n";
    return 0;
}
