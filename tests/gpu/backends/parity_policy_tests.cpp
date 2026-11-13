// SPDX-License-Identifier: MIT
//
// Pair-policy unit tests (issue #245): the tolerance table resolved from
// tests/golden/tolerance.json, the verdict rules, the report JSON and the failure diff artifact. No
// GPU is involved.

#include "parity_policy.hpp"
#include "parity_report.hpp"
#include "tessera/harness/image_compare.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace tessera::tests::gpu::backends;
using tessera::harness::RgbaImage;
using tessera::rendering::golden::DeltaEStats;
using tessera::rendering::golden::DifferenceClass;
using tessera::rendering::golden::DifferenceRegion;
using tessera::rendering::golden::DifferenceReport;
using tessera::rendering::golden::evaluate_parity;
using tessera::rendering::golden::ParityTolerance;
using tessera::rendering::golden::SsimResult;

constexpr std::uint32_t kSize = 64;
constexpr std::uint8_t kBackground = 32;
constexpr std::uint8_t kForeground = 255;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "parity_policy: " << message << "\n";
    }
    return condition;
}

std::vector<std::uint8_t> make_image(std::uint8_t value = kBackground) {
    return std::vector<std::uint8_t>(static_cast<std::size_t>(kSize) * kSize * 4, value);
}

void fill_rect(std::vector<std::uint8_t>& pixels, std::uint32_t x, std::uint32_t y,
               std::uint32_t width, std::uint32_t height, std::uint8_t value) {
    for (std::uint32_t row = y; row < y + height; ++row) {
        for (std::uint32_t column = x; column < x + width; ++column) {
            const std::size_t index = (static_cast<std::size_t>(row) * kSize + column) * 4;
            pixels[index + 0] = value;
            pixels[index + 1] = value;
            pixels[index + 2] = value;
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

PairComparison compare(const std::vector<std::uint8_t>& actual,
                       const std::vector<std::uint8_t>& expected, std::string_view candidate) {
    return compare_pair("solid_quad", candidate, "lavapipe", {kSize, kSize}, actual, expected);
}

bool test_tolerance_resolution() {
    const ParityTolerance listed = pair_tolerance("vulkan_vs_lavapipe");
    bool ok = expect(listed.edge_tolerance == 8, "vulkan_vs_lavapipe edge tolerance");
    ok = expect(listed.max_mismatch == 0.01, "vulkan_vs_lavapipe mismatch budget") && ok;
    ok = expect(listed.min_ssim == 0.99, "vulkan_vs_lavapipe SSIM floor") && ok;
    ok = expect(listed.max_delta_e == 2.0, "vulkan_vs_lavapipe deltaE ceiling") && ok;

    const ParityTolerance exact = pair_tolerance("null_vs_lavapipe");
    ok = expect(exact.edge_tolerance == 0 && exact.max_mismatch == 0.0 && exact.min_ssim == 1.0 &&
                    exact.max_delta_e == 0.0,
                "an unlisted pair is exact") &&
         ok;
    ok = expect(exact.pair == "null_vs_lavapipe", "the exact policy keeps the pair name") && ok;
    return ok;
}

bool test_identical_pair_passes() {
    const std::vector<std::uint8_t> image = make_image();
    const PairComparison comparison = compare(image, image, "vulkan");
    bool ok = expect(comparison.pair == "vulkan_vs_lavapipe", "pair key names candidate then "
                                                              "reference");
    ok = expect(comparison.passed, "identical renders pass a listed pair") && ok;
    ok = expect(comparison.reason.empty(), "a passing pair has no reason") && ok;
    ok = expect(comparison.ssim.mean == 1.0, "identical renders have SSIM 1") && ok;
    ok = expect(comparison.delta_e.mean == 0.0, "identical renders have deltaE 0") && ok;
    ok =
        expect(comparison.differences.counts.mismatched == 0, "identical renders differ nowhere") &&
        ok;
    return ok;
}

bool test_edge_rounding_passes_listed_pair() {
    std::vector<std::uint8_t> expected = make_image();
    fill_rect(expected, 8, 8, 8, 8, kForeground);
    // 28 pixels on the quad's perimeter, each one level off: edge rounding, well inside the 1%
    // budget of a 64x64 image.
    std::vector<std::uint8_t> actual = expected;
    for (std::uint32_t y = 8; y < 16; ++y) {
        for (std::uint32_t x = 8; x < 16; ++x) {
            const bool perimeter = x == 8 || x == 15 || y == 8 || y == 15;
            if (perimeter) {
                shift_pixel(actual, x, y, -1);
            }
        }
    }

    const PairComparison listed = compare(actual, expected, "vulkan");
    bool ok = expect(listed.differences.counts.edge_rounding == 28,
                     "the edge ring is classified as edge rounding");
    ok = expect(listed.differences.counts.structural == 0, "the edge ring is not structural") && ok;
    ok = expect(listed.passed, "edge rounding inside the policy passes") && ok;
    ok = expect(listed.differences.counts.mismatch_ratio() < listed.tolerance.max_mismatch,
                "the edge ratio is inside the budget") &&
         ok;

    // The same render against an unlisted pair (exact) must fail: every difference is structural.
    const PairComparison exact = compare(actual, expected, "mystery");
    ok = expect(!exact.passed, "an unlisted pair is exact and fails on a one-level difference") &&
         ok;
    ok = expect(exact.reason.find("structural difference") != std::string::npos,
                "the exact failure names the structural class") &&
         ok;
    return ok;
}

bool test_shifted_quad_fails_listed_pair() {
    std::vector<std::uint8_t> expected = make_image();
    fill_rect(expected, 8, 8, 16, 16, kForeground);
    std::vector<std::uint8_t> actual = make_image();
    fill_rect(actual, 9, 8, 16, 16, kForeground);

    const PairComparison comparison = compare(actual, expected, "vulkan");
    bool ok = expect(!comparison.passed, "a shifted quad fails even a tolerant pair");
    ok = expect(comparison.differences.counts.structural > 0, "a shifted quad is structural") && ok;
    ok = expect(comparison.reason.find("structural difference") != std::string::npos,
                "the failure reason names the structural class") &&
         ok;
    ok = expect(comparison.differences.worst_class == DifferenceClass::Structural,
                "the worst class is structural") &&
         ok;
    return ok;
}

bool test_verdict_rule_order() {
    DifferenceReport differences;
    differences.size = {4, 4};
    differences.counts.pixels = 100;

    ParityTolerance tolerance;
    tolerance.pair = "vulkan_vs_lavapipe";
    tolerance.edge_tolerance = 8;
    tolerance.max_mismatch = 0.01;
    tolerance.min_ssim = 0.99;
    tolerance.max_delta_e = 2.0;

    differences.size_mismatch = true;
    bool ok = expect(!evaluate_parity(differences, SsimResult{}, DeltaEStats{}, tolerance).passed,
                     "a size mismatch fails");
    ok = expect(evaluate_parity(differences, SsimResult{}, DeltaEStats{}, tolerance)
                        .reason.find("size mismatch") != std::string::npos,
                "a size mismatch names the rule") &&
         ok;
    differences.size_mismatch = false;

    differences.counts.structural = 3;
    differences.counts.edge_rounding = 5;
    differences.counts.mismatched = 8;
    ok = expect(evaluate_parity(differences, SsimResult{}, DeltaEStats{}, tolerance)
                        .reason.find("structural difference") != std::string::npos,
                "structural is checked before the ratio") &&
         ok;
    differences.counts.structural = 0;
    differences.counts.mismatched = 5;

    ok = expect(evaluate_parity(differences, SsimResult{}, DeltaEStats{}, tolerance)
                        .reason.find("edge-rounding ratio") != std::string::npos,
                "the edge ratio is checked before SSIM") &&
         ok;

    tolerance.max_mismatch = 0.1;
    SsimResult ssim;
    ssim.mean = 0.5;
    ok = expect(evaluate_parity(differences, ssim, DeltaEStats{}, tolerance).reason.find("SSIM") !=
                    std::string::npos,
                "SSIM is checked before deltaE") &&
         ok;

    ssim.mean = 1.0;
    DeltaEStats delta_e;
    delta_e.mean = 5.0;
    ok = expect(evaluate_parity(differences, ssim, delta_e, tolerance).reason.find("deltaE") !=
                    std::string::npos,
                "the deltaE ceiling is checked last") &&
         ok;

    delta_e.mean = 1.0;
    ok = expect(evaluate_parity(differences, ssim, delta_e, tolerance).passed,
                "a pair inside every rule passes") &&
         ok;
    return ok;
}

bool test_diff_png() {
    RgbaImage actual;
    actual.width = 2;
    actual.height = 2;
    actual.rgba = make_image(10);
    actual.rgba.resize(2 * 2 * 4);
    actual.rgba[0] = 200;
    RgbaImage expected = actual;
    expected.rgba[0] = 10;

    const std::expected<std::string, tessera::harness::ImageError> png =
        diff_png(actual, expected, 0);
    bool ok = expect(png.has_value(), "the diff PNG encodes");
    if (!png.has_value()) {
        return false;
    }
    const std::expected<RgbaImage, tessera::harness::ImageError> decoded =
        tessera::harness::decode_png(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(png->data()), png->size()));
    ok = expect(decoded.has_value(), "the diff PNG decodes") && ok;
    if (!decoded.has_value()) {
        return false;
    }
    ok = expect(decoded->width == 2 && decoded->height == 2, "the diff PNG keeps the size") && ok;
    ok = expect(decoded->rgba[0] == 255 && decoded->rgba[1] == 0 && decoded->rgba[2] == 0,
                "a mismatching pixel is solid red") &&
         ok;
    // Matching pixels are dimmed copies of the actual image (10 / 4 = 2).
    ok = expect(decoded->rgba[4] == 2 && decoded->rgba[5] == 2 && decoded->rgba[6] == 2,
                "a matching pixel is a dimmed copy") &&
         ok;
    return ok;
}

bool test_report_json() {
    ParityReport report;
    report.compared = true;
    report.passed = false;
    ArmReport arm;
    arm.name = "vulkan";
    arm.state = ArmState::Compared;
    report.arms.push_back(arm);

    PairComparison comparison;
    comparison.scene = "solid_quad";
    comparison.pair = "vulkan_vs_lavapipe";
    comparison.tolerance = pair_tolerance("vulkan_vs_lavapipe");
    comparison.differences.size = {kSize, kSize};
    comparison.differences.counts.pixels = kSize * kSize;
    comparison.differences.counts.mismatched = 4;
    comparison.differences.counts.structural = 4;
    comparison.differences.worst_class = DifferenceClass::Structural;
    comparison.differences.worst_region = DifferenceRegion{16, 16, 16, 16, 4};
    comparison.ssim.mean = 0.999;
    comparison.delta_e.mean = 0.25;
    comparison.delta_e.max = 1.0;
    comparison.passed = false;
    comparison.reason = "structural difference: 4 pixels";

    PairReport pair;
    pair.comparison = comparison;
    pair.artifacts.actual = "C:\\out\\a\"b.png";
    pair.artifacts.reference = "C:\\out\\ref.png";
    pair.artifacts.diff = "C:\\out\\diff.png";
    SceneReport scene;
    scene.scene = "solid_quad";
    scene.pairs.push_back(pair);
    report.scenes.push_back(scene);

    StructureCheck check;
    check.arm = "null";
    check.scene = "solid_quad";
    check.draw_calls = 1;
    check.vertices = 6;
    report.structure.push_back(check);

    const std::string json = parity_report_json(report);
    bool ok = expect(json == parity_report_json(report), "the report JSON is deterministic");
    ok = expect(json.find("\"reportVersion\":1") != std::string::npos, "report version") && ok;
    ok = expect(json.find("\"verdict\":\"fail\"") != std::string::npos, "report verdict") && ok;
    ok = expect(json.find("\"state\":\"compared\"") != std::string::npos, "arm state") && ok;
    ok = expect(json.find("\"pair\":\"vulkan_vs_lavapipe\"") != std::string::npos, "pair name") &&
         ok;
    ok = expect(json.find("\"structural\":4") != std::string::npos, "structural count") && ok;
    ok = expect(json.find("\"worst\":{\"class\":\"structural\",\"x\":16,\"y\":16") !=
                    std::string::npos,
                "worst region") &&
         ok;
    ok = expect(json.find("\"ssim\":0.999000") != std::string::npos, "SSIM value") && ok;
    ok = expect(json.find("C:\\\\out\\\\a\\\"b.png") != std::string::npos,
                "artifact paths are escaped") &&
         ok;
    ok = expect(json.find("\"matches_reference\":true") != std::string::npos, "structure check") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = test_tolerance_resolution() && ok;
    ok = test_identical_pair_passes() && ok;
    ok = test_edge_rounding_passes_listed_pair() && ok;
    ok = test_shifted_quad_fails_listed_pair() && ok;
    ok = test_verdict_rule_order() && ok;
    ok = test_diff_png() && ok;
    ok = test_report_json() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "parity_policy: ok\n";
    return 0;
}
