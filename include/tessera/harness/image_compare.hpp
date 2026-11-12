// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/golden/image_diff.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::harness {

// Which automation mode a frame belongs to. The mode decides the tolerance and whether a baseline
// may ever be written (docs/specs/golden-tests-v1.md section 1, ADR-009).
enum class CompareMode : std::uint8_t {
    Golden,   // software/lavapipe path: bit-exact, baselines may be updated explicitly
    GpuSmoke, // any Vulkan device: tolerant, baselines are read-only
};

[[nodiscard]] std::string_view to_string(CompareMode mode) noexcept;

// Tolerance for one comparison. A pixel mismatches when any channel differs by more than
// `per_channel_tolerance`; the image passes while mismatched/pixels <= `max_mismatch_ratio`.
struct ComparePolicy {
    std::uint32_t per_channel_tolerance = 0;
    double max_mismatch_ratio = 0.0;
};

// Golden is exact (0, 0.0). GpuSmoke approximates the spec's perceptual tolerance with a small
// per-channel delta and a pixel budget; SSIM/deltaE is not implemented.
[[nodiscard]] ComparePolicy default_policy(CompareMode mode) noexcept;

// Tightly packed RGBA8, top-left origin.
struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

enum class ImageErrorCode : std::uint8_t {
    InvalidPng,   // not decodable as PNG, or dimensions beyond kMaxImageDimension
    EncodeFailed, // PNG encoder failed
    BadImage,     // zero-sized, mismatched, or buffer shorter than width*height*4
};

struct ImageError {
    ImageErrorCode code = ImageErrorCode::InvalidPng;
    std::string message;
};

// A baseline path can be script-controlled, so the decoder bounds the pixel count it allocates.
inline constexpr std::uint32_t kMaxImageDimension = 8192;

[[nodiscard]] std::expected<RgbaImage, ImageError> decode_png(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::expected<std::string, ImageError> encode_png(const RgbaImage& image);

// Visual diff: matching pixels are dimmed copies of the actual image, mismatching pixels are solid
// red. Sizes must match; returns BadImage otherwise.
[[nodiscard]] std::expected<RgbaImage, ImageError>
make_diff_image(const RgbaImage& actual, const RgbaImage& expected,
                std::uint32_t per_channel_tolerance);

enum class CompareOutcome : std::uint8_t {
    Match,           // within policy
    Mismatch,        // same size, outside policy
    SizeMismatch,    // dimensions differ
    BaselineMissing, // no baseline to compare against
};

[[nodiscard]] std::string_view to_string(CompareOutcome outcome) noexcept;

struct CompareResult {
    CompareMode mode = CompareMode::Golden;
    CompareOutcome outcome = CompareOutcome::BaselineMissing;
    ComparePolicy policy;
    rendering::golden::ImageDiff diff;

    [[nodiscard]] bool passed() const noexcept {
        return outcome == CompareOutcome::Match;
    }
};

// Compares `actual` against `baseline` (nullptr = no baseline) under `policy`.
[[nodiscard]] CompareResult compare_images(CompareMode mode, const RgbaImage& actual,
                                           const RgbaImage* baseline, const ComparePolicy& policy);

// Stable JSON for the harness report. `baseline` is echoed as an escaped string.
[[nodiscard]] std::string compare_result_json(const CompareResult& result,
                                              std::string_view baseline);

} // namespace tessera::harness
