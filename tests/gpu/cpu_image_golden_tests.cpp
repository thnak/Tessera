// SPDX-License-Identifier: MIT
//
// Vulkan-free CPU raster-image paint golden. Builds the shared deterministic image scene
// (embedded PNG -> decode cache -> CpuCanvas::blit_texture), renders it twice to prove
// byte-identical repeat, and compares it against the checked-in baseline at tolerance 0.

#include "cpu_image_golden_tests_assets.hpp"
#include "cpu_image_scene.hpp"
#include "tessera/rendering/golden/image_diff.hpp"
#include "tessera/rendering/image/image_decode_cache.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <stb_image.h>

namespace {

using tessera::rendering::software::CpuCanvas;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "cpu-image-golden: " << message << "\n";
    }
    return condition;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "cpu-image-golden: usage: cpu_image_golden_tests <baseline.png>\n";
        return 2;
    }
    const std::string baseline_path = argv[1];

    constexpr std::uint32_t kWidth = 320;
    constexpr std::uint32_t kHeight = 160;
    CpuCanvas canvas(kWidth, kHeight);

    const auto bundle = tessera::poc::batched2d::image_fixtures::bundle();
    tessera::rendering::image::ImageDecodeCache decoded;
    const auto first = tessera::poc::batched2d::build_cpu_image_screen(canvas, bundle, decoded);
    if (!first.built) {
        std::cout << "cpu-image-golden: skipped (no embedded image fixtures)\n";
        return 0;
    }

    const std::vector<std::uint8_t> first_pixels(canvas.pixels().begin(), canvas.pixels().end());
    const auto decode_stats = decoded.stats();
    std::cout << "cpu-image-golden: draws=" << first.draws << " decodes=" << decode_stats.decodes
              << " misses=" << decode_stats.misses << "\n";

    // Deterministic repeat: a second full render must be byte-identical before comparing goldens.
    CpuCanvas repeat_canvas(kWidth, kHeight);
    const auto repeat =
        tessera::poc::batched2d::build_cpu_image_screen(repeat_canvas, bundle, decoded);
    const std::vector<std::uint8_t> repeat_pixels(repeat_canvas.pixels().begin(),
                                                  repeat_canvas.pixels().end());
    bool ok = expect(repeat.built, "the repeat render built the scene") &&
              expect(first_pixels == repeat_pixels, "two CPU image renders are byte-identical") &&
              expect(decode_stats.decodes == 1U, "the shared image decodes once for all nodes") &&
              expect(decode_stats.hits > 0U, "later nodes hit the decode cache");

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* raw = stbi_load(baseline_path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (raw == nullptr) {
        std::cerr << "cpu-image-golden: cannot read baseline: " << baseline_path << "\n";
        return 2;
    }
    const std::vector<std::uint8_t> expected(raw,
                                             raw + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(raw);

    ok = expect(static_cast<std::uint32_t>(width) == kWidth &&
                    static_cast<std::uint32_t>(height) == kHeight,
                "baseline dimensions match the scene") &&
         ok;
    if (width == static_cast<int>(kWidth) && height == static_cast<int>(kHeight)) {
        const auto diff = tessera::rendering::golden::compare_rgba(
            tessera::rendering::golden::ImageSize{kWidth, kHeight}, first_pixels,
            tessera::rendering::golden::ImageSize{kWidth, kHeight}, expected, 0);
        if (diff.mismatched != 0) {
            std::cerr << "cpu-image-golden: baseline mismatch (" << diff.mismatched << "/"
                      << (kWidth * kHeight) << " pixels)\n";
        }
        ok = expect(diff.mismatched == 0, "CPU image render matches the baseline at tolerance 0") &&
             ok;
    }

    if (!ok) {
        std::cerr << "cpu-image-golden: FAILED\n";
        return 1;
    }
    std::cout << "cpu-image-golden: ok\n";
    return 0;
}
