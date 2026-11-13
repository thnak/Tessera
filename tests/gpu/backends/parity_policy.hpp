// SPDX-License-Identifier: MIT
//
// Pair policy for the cross-backend parity harness: resolve the per-backend-pair tolerance from
// tests/golden/tolerance.json (generated into parity_tolerances.hpp at configure time) and apply
// the perceptual metric and the difference classification to one rendered pair.

#pragma once

#include "tessera/rendering/golden/difference_class.hpp"
#include "tessera/rendering/golden/parity.hpp"
#include "tessera/rendering/golden/perceptual.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tessera::tests::gpu::backends {

struct PairComparison {
    std::string scene;
    std::string pair; // "<candidate>_vs_<reference>"
    rendering::golden::ParityTolerance tolerance;
    rendering::golden::DifferenceReport differences;
    rendering::golden::SsimResult ssim;
    rendering::golden::DeltaEStats delta_e;
    bool passed = false;
    std::string reason;
};

// The policy from tolerance.json; a pair that is not listed resolves to exact.
[[nodiscard]] rendering::golden::ParityTolerance pair_tolerance(std::string_view pair);

// Compares two RGBA8 images of the same scene. `candidate` and `reference` are the backend names
// that form the pair key; the images must have the same dimensions (a mismatch fails the pair).
[[nodiscard]] PairComparison compare_pair(std::string_view scene, std::string_view candidate,
                                          std::string_view reference,
                                          rendering::golden::ImageSize size,
                                          std::span<const std::uint8_t> actual_pixels,
                                          std::span<const std::uint8_t> expected_pixels);

} // namespace tessera::tests::gpu::backends
