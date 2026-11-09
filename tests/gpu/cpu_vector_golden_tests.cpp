// SPDX-License-Identifier: MIT
//
// Vulkan-free CPU vector paint golden. Builds the shared deterministic vector scene
// (embedded SVG assets -> CPU raster -> textured-quad blit), renders it twice to prove
// byte-identical repeat, and compares it against the checked-in baseline at tolerance 0. Skips
// cleanly when the build has ThorVG OFF (the scene prints the documented skip line).

#include "cpu_vector_golden_tests_assets.hpp"
#include "cpu_vector_scene.hpp"
#include "tessera/rendering/golden/image_diff.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <stb_image.h>

namespace {

using tessera::rendering::software::CpuCanvas;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "cpu-vector-golden: " << message << "\n";
    }
    return condition;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "cpu-vector-golden: usage: cpu_vector_golden_tests <baseline.png>\n";
        return 2;
    }
    const std::string baseline_path = argv[1];

    constexpr std::uint32_t kWidth = 320;
    constexpr std::uint32_t kHeight = 160;
    CpuCanvas canvas(kWidth, kHeight);

    const auto bundle = tessera::poc::batched2d::fixtures::bundle();
    const auto first = tessera::poc::batched2d::build_cpu_vector_screen(canvas, bundle);
    if (!first.built) {
        std::cout << "cpu-vector-golden: skipped (ThorVG backend unavailable)\n";
        return 0; // CTest SKIP_REGULAR_EXPRESSION treats this as a clean skip
    }

    const std::vector<std::uint8_t> first_pixels(canvas.pixels().begin(), canvas.pixels().end());
    std::cout << "cpu-vector-golden: draws=" << first.draws << " raster_ms=" << first.raster_ms
              << " blit_ms=" << first.blit_ms << "\n";

    // Deterministic repeat: a second full render must be byte-identical before comparing goldens.
    CpuCanvas repeat_canvas(kWidth, kHeight);
    const auto repeat = tessera::poc::batched2d::build_cpu_vector_screen(repeat_canvas, bundle);
    const std::vector<std::uint8_t> repeat_pixels(repeat_canvas.pixels().begin(),
                                                  repeat_canvas.pixels().end());
    bool ok = expect(repeat.built, "the repeat render built the scene") &&
              expect(first_pixels == repeat_pixels, "two CPU vector renders are byte-identical");

    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load(baseline_path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (decoded == nullptr) {
        std::cerr << "cpu-vector-golden: cannot read baseline: " << baseline_path << "\n";
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
            std::cerr << "cpu-vector-golden: baseline mismatch (" << diff.mismatched << "/"
                      << (kWidth * kHeight) << " pixels)\n";
        }
        ok =
            expect(diff.mismatched == 0, "CPU vector render matches the baseline at tolerance 0") &&
            ok;
    }

    if (!ok) {
        std::cerr << "cpu-vector-golden: FAILED\n";
        return 1;
    }
    std::cout << "cpu-vector-golden: ok\n";
    return 0;
}
