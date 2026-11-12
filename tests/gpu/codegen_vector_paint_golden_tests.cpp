// SPDX-License-Identifier: MIT
//
// CPU paint golden for the codegen-synthesized `Path` vector payload. Builds the generated
// `vicon::gen::VectorIcon` component (tests/golden/codegen/vector/), gives its nodes bounds, and
// paints the scene through `CpuPaintSink`: the payload's asset name resolves through the generated
// bundle, the SVG decodes, and the path rasterizes into the content box. Two renders must be
// byte-identical and the result must match the committed baseline at tolerance 0.
//
// The document is drawn with `fill="#2e8b57"` and a dark stroke; the defaults-only node
// (`fill`/`stroke` `"none"`) has a decoded shape with no paint, so its box must stay background —
// that pins both the explicit and the documented-default document.
//
// Skips cleanly when the build has no ThorVG (the paint chain prints the documented skip line).
// Regenerate deliberately: `codegen_vector_paint_golden_tests --write <baseline.png>`.

#include "VectorIcon.hpp"
#include "cpu_paint_sink.hpp"
#include "cpu_scene_styles.hpp"
#include "tessera/rendering/golden/image_diff.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "vicon_vector_assets.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <stb_image.h>
#include <stb_image_write.h>

namespace {

using tessera::rendering::software::CpuCanvas;
using tessera::ui::Node;

constexpr std::uint32_t kWidth = 96;
constexpr std::uint32_t kHeight = 64;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "codegen-vector-paint-golden: " << message << "\n";
    }
    return condition;
}

// Builds the generated component into `root` and gives the two `Path` nodes their boxes (layout is
// the host's job; a golden fixes the boxes directly so the render is deterministic).
void build_scene(Node& root) {
    vicon::gen::VectorIcon component;
    component.build(root);
    Node* view = root.children().empty() ? nullptr : root.children()[0].get();
    if (view == nullptr || view->children().size() != 2) {
        return;
    }
    view->children()[0]->set_bounds({8.0F, 8.0F, 48.0F, 48.0F});
    view->children()[1]->set_bounds({64.0F, 8.0F, 24.0F, 24.0F});
}

struct RenderResult {
    bool built = false;
    std::uint32_t draws = 0;
};

RenderResult render(CpuCanvas& canvas) {
    RenderResult result;
    auto rasterizer = tessera::rendering::path::make_path_rasterizer(
        tessera::rendering::path::PathBackend::thorvg);
    if (rasterizer == nullptr) {
        return result;
    }
    tessera::rendering::path::SvgDocumentCache documents;
    const auto bundle = vicon::gen::vector_assets::bundle();
    tessera::poc::batched2d::CpuPaintSink sink(
        canvas, nullptr, nullptr,
        tessera::poc::batched2d::CpuVectorConfig{&bundle, &documents, rasterizer.get(), 1.0F});

    Node root(900);
    root.set_bounds({0.0F, 0.0F, static_cast<float>(kWidth), static_cast<float>(kHeight)});
    build_scene(root);

    tessera::poc::batched2d::CpuSceneStyles styles;
    styles.set(900, tessera::poc::batched2d::cpu_style({{"background-color", "#0F172A"}}));
    canvas.clear(tessera::rendering::rhi::rgba8(11, 18, 32));
    (void)tessera::ui::paint::paint_scene(root, styles, sink);
    result.built = true;
    result.draws = sink.vector_draws();
    return result;
}

[[nodiscard]] std::uint32_t pixel_at(const std::vector<std::uint8_t>& pixels, std::uint32_t x,
                                     std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * kWidth + x) * 4;
    if (index + 3 >= pixels.size()) {
        return 0;
    }
    return (static_cast<std::uint32_t>(pixels[index]) << 24U) |
           (static_cast<std::uint32_t>(pixels[index + 1]) << 16U) |
           (static_cast<std::uint32_t>(pixels[index + 2]) << 8U) |
           static_cast<std::uint32_t>(pixels[index + 3]);
}

// r,g,b,a packed the same way as `pixel_at` (the canvas stores straight-alpha RGBA8).
constexpr std::uint32_t pack(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (static_cast<std::uint32_t>(r) << 24U) | (static_cast<std::uint32_t>(g) << 16U) |
           (static_cast<std::uint32_t>(b) << 8U) | 0xFFU;
}

} // namespace

int main(int argc, char** argv) {
    const bool write = argc > 2 && std::string_view(argv[1]) == "--write";
    if (argc < 2 || (write && argc < 3)) {
        std::cerr << "usage: codegen_vector_paint_golden_tests <baseline.png>\n"
                     "       codegen_vector_paint_golden_tests --write <baseline.png>\n";
        return 2;
    }
    const std::string baseline_path = write ? argv[2] : argv[1];

    if (!tessera::rendering::path::svg_backend_available()) {
        std::cout << "codegen-vector-paint-golden: skipped (ThorVG backend unavailable)\n";
        return 0; // CTest SKIP_REGULAR_EXPRESSION treats this as a clean skip
    }

    CpuCanvas canvas(kWidth, kHeight);
    const RenderResult first = render(canvas);
    if (!first.built) {
        std::cout << "codegen-vector-paint-golden: skipped (no CPU path rasterizer)\n";
        return 0;
    }
    const std::vector<std::uint8_t> first_pixels(canvas.pixels().begin(), canvas.pixels().end());

    // Deterministic repeat before trusting either the baseline or the pixel probes.
    CpuCanvas repeat_canvas(kWidth, kHeight);
    const RenderResult repeat = render(repeat_canvas);
    const std::vector<std::uint8_t> repeat_pixels(repeat_canvas.pixels().begin(),
                                                  repeat_canvas.pixels().end());
    bool ok = expect(repeat.built, "the repeat render built the scene") &&
              expect(first_pixels == repeat_pixels, "two renders are byte-identical") &&
              expect(first.draws == 2, "both `Path` documents reach the sink as one quad each");

    // Pixel probes: the filled triangle's interior (vertices scale to (16,16)/(48,16)/(48,48)) is
    // its fill color, and the defaults-only node's box stays background because its document has no
    // painted shape.
    const std::uint32_t background = pack(15, 23, 42);
    const std::uint32_t fill = pack(46, 139, 87);
    ok = expect(pixel_at(first_pixels, 40, 30) == fill,
                "the triangle interior is the document's fill color") &&
         expect(pixel_at(first_pixels, 20, 40) == background,
                "outside the triangle the content box stays background") &&
         expect(pixel_at(first_pixels, 76, 20) == background,
                "a `Path` with default fill/stroke paints nothing") &&
         expect(pixel_at(first_pixels, 2, 2) == background,
                "the node's box outside the content area stays background") &&
         ok;

    if (write) {
        const int written = stbi_write_png(baseline_path.c_str(), static_cast<int>(kWidth),
                                           static_cast<int>(kHeight), 4, first_pixels.data(),
                                           static_cast<int>(kWidth) * 4);
        if (written == 0) {
            std::cerr << "codegen-vector-paint-golden: cannot write " << baseline_path << "\n";
            return 2;
        }
        std::cout << "codegen-vector-paint-golden: wrote " << baseline_path << "\n";
        return ok ? 0 : 1;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(baseline_path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (decoded == nullptr) {
        std::cerr << "codegen-vector-paint-golden: cannot read baseline: " << baseline_path << "\n";
        return 2;
    }
    const std::vector<std::uint8_t> expected(decoded, decoded + static_cast<std::size_t>(width) *
                                                                    height * 4);
    stbi_image_free(decoded);

    ok = expect(static_cast<std::uint32_t>(width) == kWidth &&
                    static_cast<std::uint32_t>(height) == kHeight,
                "baseline dimensions match the scene") &&
         ok;
    if (width == static_cast<int>(kWidth) && height == static_cast<int>(kHeight)) {
        const auto diff = tessera::rendering::golden::compare_rgba(
            tessera::rendering::golden::ImageSize{kWidth, kHeight}, first_pixels,
            tessera::rendering::golden::ImageSize{static_cast<std::uint32_t>(width),
                                                  static_cast<std::uint32_t>(height)},
            expected, 0);
        if (diff.mismatched != 0) {
            std::cerr << "codegen-vector-paint-golden: baseline mismatch (" << diff.mismatched
                      << "/" << (kWidth * kHeight) << " pixels)\n";
        }
        ok = expect(diff.mismatched == 0, "the render matches the baseline at tolerance 0") && ok;
    }

    if (!ok) {
        std::cerr << "codegen-vector-paint-golden: FAILED\n";
        return 1;
    }
    std::cout << "codegen-vector-paint-golden: ok (draws=" << first.draws << ")\n";
    return 0;
}
