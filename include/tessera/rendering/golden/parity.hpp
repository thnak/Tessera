// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/golden/difference_class.hpp"
#include "tessera/rendering/golden/perceptual.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace tessera::rendering::golden {

// Per-backend-pair parity policy, resolved from tests/golden/tolerance.json (schema 2) and applied
// to two images of the same scene rendered by different backends (issue #245). The pair key is
// "<candidate>_vs_<reference>", for example "vulkan_vs_lavapipe". A pair that is not listed is
// exact: no differing pixel is acceptable at all.
struct ParityTolerance {
    std::string_view pair;
    // Largest per-channel delta that may count as edge rounding; see DifferenceClassOptions.
    std::uint32_t edge_tolerance = 0;
    // Largest fraction of pixels that may be classified as edge rounding.
    double max_mismatch = 0.0;
    // Floor for the mean SSIM on luma; 1.0 means "identical".
    double min_ssim = 1.0;
    // Ceiling for the mean CIE76 deltaE; 0.0 means "identical".
    double max_delta_e = 0.0;
};

// The exact policy an unlisted pair resolves to.
inline constexpr ParityTolerance kExactParityTolerance{};

struct ParityVerdict {
    bool passed = false;
    // The first failing rule, in stable text with the measured value and the policy value. Empty
    // on a pass, so the report never invents a reason.
    std::string reason;
};

// Applies `tolerance` to the three measures. Rules are checked in this order, so the reported
// reason is deterministic: size mismatch, structural pixels (never tolerated), edge-rounding
// ratio, mean SSIM floor, mean deltaE ceiling.
[[nodiscard]] ParityVerdict evaluate_parity(const DifferenceReport& differences,
                                            const SsimResult& ssim, const DeltaEStats& delta_e,
                                            const ParityTolerance& tolerance);

// The table entry for `pair`, or kExactParityTolerance when the pair is not listed.
[[nodiscard]] ParityTolerance resolve_parity_tolerance(std::span<const ParityTolerance> table,
                                                       std::string_view pair);

} // namespace tessera::rendering::golden
