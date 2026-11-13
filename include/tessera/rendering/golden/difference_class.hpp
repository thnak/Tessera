// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/golden/image_diff.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace tessera::rendering::golden {

// Two classes a cross-backend image difference can fall into (issue #245):
//
//   * edge rounding - a small per-channel delta that sits within `edge_radius` pixels of a luma
//     gradient in either image. Antialiasing, SDF coverage and texture filtering round differently
//     between backends, but only where the image already changes quickly, and only a little.
//   * structural   - everything else: a delta too large for edge rounding, a difference in a flat
//     region (wrong color), or a region the other backend did not draw at all (missing draw /
//     shifted geometry).
//
// The classification never loosens the per-channel rule: a pixel is a difference as soon as any
// channel differs, and only the class decides whether a tolerance budget may absorb it.
enum class DifferenceClass : std::uint8_t {
    None,
    EdgeRounding,
    Structural,
};

[[nodiscard]] std::string_view to_string(DifferenceClass value) noexcept;

// An axis-aligned region of the image; `pixels` is how many counted pixels fall inside it.
struct DifferenceRegion {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t pixels = 0;

    [[nodiscard]] bool empty() const noexcept {
        return width == 0 || height == 0 || pixels == 0;
    }
};

struct DifferenceCounts {
    std::size_t pixels = 0;
    std::size_t mismatched = 0;
    std::size_t edge_rounding = 0;
    std::size_t structural = 0;

    [[nodiscard]] double mismatch_ratio() const noexcept;
    [[nodiscard]] double edge_ratio() const noexcept;
    [[nodiscard]] double structural_ratio() const noexcept;
};

struct DifferenceClassOptions {
    // Largest per-channel delta that may count as edge rounding. A pair that is not listed in
    // tests/golden/tolerance.json resolves to 0, which means "any difference is structural" and
    // therefore exact.
    std::uint32_t edge_tolerance = 0;
    // Chebyshev radius around a gradient pixel that still counts as "on an edge".
    std::uint32_t edge_radius = 2;
    // Minimum luma step to a 4-neighbour that makes a pixel a gradient pixel.
    std::uint32_t gradient_threshold = 8;
    // Side of the grid cell used to report the worst region (largest count wins; ties go to the
    // smallest y then the smallest x, so the report is deterministic).
    std::uint32_t block = 16;
};

struct DifferenceReport {
    ImageSize size;
    DifferenceCounts counts;
    DifferenceClass worst_class = DifferenceClass::None;
    DifferenceRegion worst_region;
    bool size_mismatch = false;
};

// Classifies every differing pixel of two tightly packed RGBA8 images. Alpha is compared like the
// per-channel rule does (compare_rgba). A size mismatch reports `size_mismatch` and no counts.
[[nodiscard]] DifferenceReport classify_differences(ImageSize actual_size,
                                                    std::span<const std::uint8_t> actual_pixels,
                                                    ImageSize expected_size,
                                                    std::span<const std::uint8_t> expected_pixels,
                                                    const DifferenceClassOptions& options = {});

} // namespace tessera::rendering::golden
