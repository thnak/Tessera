// SPDX-License-Identifier: MIT

#include "parity_policy.hpp"
#include "parity_tolerances.hpp"

#include <string>

namespace tessera::tests::gpu::backends {

using namespace tessera::rendering;

golden::ParityTolerance pair_tolerance(std::string_view pair) {
    return golden::resolve_parity_tolerance(kParityPairTolerances, pair);
}

PairComparison compare_pair(std::string_view scene, std::string_view candidate,
                            std::string_view reference, golden::ImageSize size,
                            std::span<const std::uint8_t> actual_pixels,
                            std::span<const std::uint8_t> expected_pixels) {
    PairComparison comparison;
    comparison.scene = std::string(scene);
    comparison.pair = std::string(candidate) + "_vs_" + std::string(reference);
    comparison.tolerance = pair_tolerance(comparison.pair);

    golden::DifferenceClassOptions options;
    options.edge_tolerance = comparison.tolerance.edge_tolerance;
    comparison.differences =
        golden::classify_differences(size, actual_pixels, size, expected_pixels, options);
    comparison.ssim = golden::compute_ssim(size, actual_pixels, size, expected_pixels);
    comparison.delta_e = golden::compute_delta_e(size, actual_pixels, size, expected_pixels);

    const golden::ParityVerdict verdict = golden::evaluate_parity(
        comparison.differences, comparison.ssim, comparison.delta_e, comparison.tolerance);
    comparison.passed = verdict.passed;
    comparison.reason = verdict.reason;
    return comparison;
}

} // namespace tessera::tests::gpu::backends
