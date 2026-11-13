// SPDX-License-Identifier: MIT
//
// Synthetic-image unit tests for the difference classes (issue #245). A one-pixel shift of a quad
// must be structural; a one-level rounding difference on the edges must be edge rounding and pass
// an exact-for-the-interior policy; a wrong colour in a flat region must be structural even when it
// is only 8 levels and sits inside the pair's per-channel allowance.

#include "tessera/rendering/golden/difference_class.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {

using tessera::rendering::golden::classify_differences;
using tessera::rendering::golden::DifferenceClass;
using tessera::rendering::golden::DifferenceClassOptions;
using tessera::rendering::golden::DifferenceReport;
using tessera::rendering::golden::ImageSize;

constexpr std::uint32_t kSize = 24;
constexpr std::uint8_t kBackground = 32;
constexpr std::uint8_t kForeground = 255;

struct Rgba {
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "difference_class: " << message << "\n";
    }
    return condition;
}

std::vector<std::uint8_t> make_image(std::uint8_t value = kBackground) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(kSize) * kSize * 4, value);
}

void fill_rect(std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y,
               std::uint32_t width, std::uint32_t height, Rgba color) {
    for (std::uint32_t row = y; row < y + height; ++row) {
        for (std::uint32_t column = x; column < x + width; ++column) {
            const std::size_t index = (static_cast<std::size_t>(row) * kSize + column) * 4;
            pixels[index + 0] = color.r;
            pixels[index + 1] = color.g;
            pixels[index + 2] = color.b;
            pixels[index + 3] = 255;
        }
    }
}

void shift_pixel(std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y, int delta) {
    const std::size_t index = (static_cast<std::size_t>(y) * kSize + x) * 4;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const int value = static_cast<int>(pixels[index + channel]) + delta;
        pixels[index + channel] = static_cast<std::uint8_t>(value < 0 ? 0 : value);
    }
}

bool inside_rect(std::uint32_t x, std::uint32_t y, std::uint32_t rx, std::uint32_t ry,
                 std::uint32_t width, std::uint32_t height) {
    return x >= rx && x < rx + width && y >= ry && y < ry + height;
}

DifferenceReport classify(const std::vector<std::uint8_t>& actual,
                          const std::vector<std::uint8_t>& expected,
                          DifferenceClassOptions options = {}) {
    return classify_differences({kSize, kSize}, actual, {kSize, kSize}, expected, options);
}

bool test_identical() {
    const std::vector<std::uint8_t> image = make_image();
    const DifferenceReport report = classify(image, image);
    bool ok = expect(report.counts.mismatched == 0, "identical images differ nowhere");
    ok = expect(report.counts.edge_rounding == 0 && report.counts.structural == 0,
                "identical images have no classified pixels") &&
         ok;
    ok = expect(report.worst_class == DifferenceClass::None, "identical images have no worst "
                                                             "class") &&
         ok;
    ok = expect(report.worst_region.empty(), "identical images have no worst region") && ok;
    ok = expect(!report.size_mismatch, "identical images are not a size mismatch") && ok;
    return ok;
}

bool test_shifted_quad_is_structural() {
    std::vector<std::uint8_t> expected = make_image();
    fill_rect(expected, 4, 4, 12, 12, Rgba{kForeground, kForeground, kForeground});
    std::vector<std::uint8_t> actual = make_image();
    fill_rect(actual, 5, 4, 12, 12, Rgba{kForeground, kForeground, kForeground});

    const DifferenceReport report = classify(actual, expected);
    bool ok = expect(report.counts.structural > 0, "a one-pixel shift is structural");
    ok = expect(report.counts.edge_rounding == 0,
                "a one-pixel shift is never classified as edge rounding") &&
         ok;
    ok = expect(report.worst_class == DifferenceClass::Structural, "worst class is structural") &&
         ok;
    ok = expect(!report.worst_region.empty(), "the structural difference names a region") && ok;
    ok = expect(inside_rect(report.worst_region.x, report.worst_region.y, 0, 0, 16, 16),
                "the worst structural region covers the shifted edge") &&
         ok;
    return ok;
}

bool test_edge_rounding_is_classified() {
    std::vector<std::uint8_t> expected = make_image();
    fill_rect(expected, 4, 4, 12, 12, Rgba{kForeground, kForeground, kForeground});

    // One-level rounding on the ring that touches the quad's luma gradient: the perimeter pixels
    // inside the quad and the background pixels just outside it each move by one level, which is
    // what AA/rounding drift looks like.
    std::vector<std::uint8_t> actual = expected;
    for (std::uint32_t y = 3; y <= 16; ++y) {
        for (std::uint32_t x = 3; x <= 16; ++x) {
            const bool inside = inside_rect(x, y, 4, 4, 12, 12);
            const bool on_quad_perimeter = inside && (x == 4 || x == 15 || y == 4 || y == 15);
            const bool on_outer_ring = !inside && (x == 3 || x == 16 || y == 3 || y == 16);
            if (on_quad_perimeter || on_outer_ring) {
                shift_pixel(actual, x, y, -1);
            }
        }
    }

    DifferenceClassOptions options;
    options.edge_tolerance = 2;
    const DifferenceReport report = classify(actual, expected, options);
    bool ok = expect(report.counts.mismatched > 0, "the rounding difference is detected");
    ok = expect(report.counts.edge_rounding > 0,
                "a one-level difference on a luma edge is edge rounding") &&
         ok;
    ok = expect(report.counts.structural == 0,
                "a one-level difference on a luma edge is not structural") &&
         ok;
    ok = expect(report.worst_class == DifferenceClass::EdgeRounding, "worst class is edge "
                                                                     "rounding") &&
         ok;

    // The same image under the exact policy (edge tolerance 0) is structural, which is what makes
    // an unlisted pair exact.
    const DifferenceReport exact = classify(actual, expected);
    ok = expect(exact.counts.structural > 0 && exact.counts.edge_rounding == 0,
                "with no edge allowance the rounding difference is structural") &&
         ok;
    return ok;
}

bool test_wrong_color_in_flat_region_is_structural() {
    std::vector<std::uint8_t> expected = make_image();
    fill_rect(expected, 4, 4, 12, 12, Rgba{kForeground, kForeground, kForeground});
    std::vector<std::uint8_t> actual = make_image();
    fill_rect(actual, 4, 4, 12, 12, Rgba{kForeground, kForeground, 247});

    DifferenceClassOptions options;
    options.edge_tolerance = 8;
    const DifferenceReport report = classify(actual, expected, options);
    bool ok = expect(report.counts.structural > 0,
                     "an 8-level wrong colour in a flat region is structural");
    ok = expect(report.worst_class == DifferenceClass::Structural, "worst class is structural") &&
         ok;
    // The flat interior is away from the quad's gradient, so its differences cannot be rounding.
    ok = expect(report.worst_region.width > 0 && report.worst_region.height > 0,
                "the worst structural region has an area") &&
         ok;
    return ok;
}

bool test_missing_draw_is_structural() {
    std::vector<std::uint8_t> expected = make_image();
    fill_rect(expected, 4, 4, 12, 12, Rgba{kForeground, kForeground, kForeground});
    const std::vector<std::uint8_t> actual = make_image();

    DifferenceClassOptions options;
    options.edge_tolerance = 8;
    const DifferenceReport report = classify(actual, expected, options);
    bool ok = expect(report.counts.structural == 144, "a missing 12x12 draw is 144 structural "
                                                      "pixels");
    ok = expect(report.counts.edge_rounding == 0, "a missing draw has no edge rounding") && ok;
    return ok;
}

bool test_worst_region_tie_break_is_deterministic() {
    const std::vector<std::uint8_t> expected = make_image();
    std::vector<std::uint8_t> actual = expected;
    DifferenceClassOptions options;
    options.block = 8;
    // Two structural pixels in block (2,1) (x 16..23, y 8..15) and two in block (0,2)
    // (x 0..7, y 16..23): equal counts, and the scan order picks the smallest y block, so the
    // region at (16,8) must win.
    shift_pixel(actual, 17, 9, 100);
    shift_pixel(actual, 18, 10, 100);
    shift_pixel(actual, 1, 17, 100);
    shift_pixel(actual, 2, 18, 100);

    const DifferenceReport report = classify(actual, expected, options);
    bool ok = expect(report.counts.structural == 4, "four structural pixels are counted");
    ok = expect(report.worst_region.x == 16 && report.worst_region.y == 8,
                "a count tie goes to the smallest y then the smallest x") &&
         ok;
    ok = expect(report.worst_region.pixels == 2, "the worst region reports its own count") && ok;
    return ok;
}

bool test_size_mismatch() {
    const std::vector<std::uint8_t> actual = make_image();
    const std::vector<std::uint8_t> expected(static_cast<std::size_t>(8) * 8 * 4, 0);
    const DifferenceReport report = classify_differences({kSize, kSize}, actual, {8, 8}, expected);
    return expect(report.size_mismatch, "different sizes report a size mismatch") &&
           expect(report.counts.mismatched == 0, "a size mismatch counts no pixels");
}

bool test_result_is_repeatable() {
    std::vector<std::uint8_t> expected = make_image();
    fill_rect(expected, 4, 4, 12, 12, Rgba{kForeground, kForeground, kForeground});
    std::vector<std::uint8_t> actual = make_image();
    fill_rect(actual, 5, 4, 12, 12, Rgba{kForeground, kForeground, kForeground});
    const DifferenceReport first = classify(actual, expected);
    const DifferenceReport second = classify(actual, expected);
    return expect(first.counts.structural == second.counts.structural &&
                      first.worst_region.x == second.worst_region.x &&
                      first.worst_region.y == second.worst_region.y,
                  "classification is deterministic");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_identical() && ok;
    ok = test_shifted_quad_is_structural() && ok;
    ok = test_edge_rounding_is_classified() && ok;
    ok = test_wrong_color_in_flat_region_is_structural() && ok;
    ok = test_missing_draw_is_structural() && ok;
    ok = test_worst_region_tie_break_is_deterministic() && ok;
    ok = test_size_mismatch() && ok;
    ok = test_result_is_repeatable() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "difference_class: ok\n";
    return 0;
}
