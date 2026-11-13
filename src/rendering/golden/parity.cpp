// SPDX-License-Identifier: MIT

#include "tessera/rendering/golden/parity.hpp"

#include <string>

namespace tessera::rendering::golden {

ParityVerdict evaluate_parity(const DifferenceReport& differences, const SsimResult& ssim,
                              const DeltaEStats& delta_e, const ParityTolerance& tolerance) {
    ParityVerdict verdict;
    if (differences.size_mismatch) {
        verdict.reason = "size mismatch: " + std::to_string(differences.size.width) + "x" +
                         std::to_string(differences.size.height) + " against the reference";
        return verdict;
    }
    if (differences.counts.structural > 0) {
        verdict.reason = "structural difference: " + std::to_string(differences.counts.structural) +
                         " pixels (worst region " + std::to_string(differences.worst_region.x) +
                         "," + std::to_string(differences.worst_region.y) + " " +
                         std::to_string(differences.worst_region.width) + "x" +
                         std::to_string(differences.worst_region.height) + ")";
        return verdict;
    }
    if (differences.counts.edge_ratio() > tolerance.max_mismatch) {
        verdict.reason = "edge-rounding ratio " + std::to_string(differences.counts.edge_ratio()) +
                         " exceeds " + std::to_string(tolerance.max_mismatch);
        return verdict;
    }
    if (ssim.mean < tolerance.min_ssim) {
        verdict.reason =
            "SSIM " + std::to_string(ssim.mean) + " is below " + std::to_string(tolerance.min_ssim);
        return verdict;
    }
    if (delta_e.mean > tolerance.max_delta_e) {
        verdict.reason = "mean deltaE " + std::to_string(delta_e.mean) + " exceeds " +
                         std::to_string(tolerance.max_delta_e);
        return verdict;
    }
    verdict.passed = true;
    return verdict;
}

ParityTolerance resolve_parity_tolerance(std::span<const ParityTolerance> table,
                                         std::string_view pair) {
    for (const ParityTolerance& tolerance : table) {
        if (tolerance.pair == pair) {
            return tolerance;
        }
    }
    ParityTolerance exact = kExactParityTolerance;
    exact.pair = pair;
    return exact;
}

} // namespace tessera::rendering::golden
