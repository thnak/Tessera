// SPDX-License-Identifier: MIT

#include "tessera/harness/image_compare.hpp"

#include <cstddef>
#include <string>

namespace tessera::harness {
namespace {

bool valid(const RgbaImage& image) {
    return image.width > 0 && image.height > 0 &&
           image.rgba.size() >= static_cast<std::size_t>(image.width) * image.height * 4;
}

void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20U) {
                constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[(static_cast<unsigned char>(c) >> 4U) & 0xFU]);
                out.push_back(kHex[static_cast<unsigned char>(c) & 0xFU]);
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

} // namespace

std::string_view to_string(CompareMode mode) noexcept {
    return mode == CompareMode::Golden ? "golden" : "gpu-smoke";
}

std::string_view to_string(CompareOutcome outcome) noexcept {
    switch (outcome) {
    case CompareOutcome::Match:
        return "match";
    case CompareOutcome::Mismatch:
        return "mismatch";
    case CompareOutcome::SizeMismatch:
        return "size-mismatch";
    case CompareOutcome::BaselineMissing:
        return "baseline-missing";
    }
    return "baseline-missing";
}

ComparePolicy default_policy(CompareMode mode) noexcept {
    if (mode == CompareMode::Golden) {
        return ComparePolicy{0, 0.0};
    }
    return ComparePolicy{8, 0.02};
}

std::expected<RgbaImage, ImageError> make_diff_image(const RgbaImage& actual,
                                                     const RgbaImage& expected,
                                                     std::uint32_t per_channel_tolerance) {
    if (!valid(actual) || !valid(expected) || actual.width != expected.width ||
        actual.height != expected.height) {
        return std::unexpected(
            ImageError{ImageErrorCode::BadImage, "diff image needs two equally sized images"});
    }
    RgbaImage out;
    out.width = actual.width;
    out.height = actual.height;
    const std::size_t pixels = static_cast<std::size_t>(actual.width) * actual.height;
    out.rgba.resize(pixels * 4);
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        const std::size_t base = pixel * 4;
        bool mismatched = false;
        for (std::size_t channel = 0; channel < 4; ++channel) {
            const auto a = static_cast<std::uint32_t>(actual.rgba[base + channel]);
            const auto b = static_cast<std::uint32_t>(expected.rgba[base + channel]);
            if ((a > b ? a - b : b - a) > per_channel_tolerance) {
                mismatched = true;
            }
        }
        if (mismatched) {
            out.rgba[base] = 255;
            out.rgba[base + 1] = 0;
            out.rgba[base + 2] = 0;
        } else {
            out.rgba[base] = static_cast<std::uint8_t>(actual.rgba[base] / 4U);
            out.rgba[base + 1] = static_cast<std::uint8_t>(actual.rgba[base + 1] / 4U);
            out.rgba[base + 2] = static_cast<std::uint8_t>(actual.rgba[base + 2] / 4U);
        }
        out.rgba[base + 3] = 255;
    }
    return out;
}

CompareResult compare_images(CompareMode mode, const RgbaImage& actual, const RgbaImage* baseline,
                             const ComparePolicy& policy) {
    CompareResult result;
    result.mode = mode;
    result.policy = policy;
    result.diff.actual = {actual.width, actual.height};
    if (baseline == nullptr) {
        result.outcome = CompareOutcome::BaselineMissing;
        return result;
    }
    result.diff = rendering::golden::compare_rgba({actual.width, actual.height}, actual.rgba,
                                                  {baseline->width, baseline->height},
                                                  baseline->rgba, policy.per_channel_tolerance);
    if (result.diff.size_mismatch) {
        result.outcome = CompareOutcome::SizeMismatch;
    } else if (rendering::golden::within_tolerance(result.diff, policy.max_mismatch_ratio)) {
        result.outcome = CompareOutcome::Match;
    } else {
        result.outcome = CompareOutcome::Mismatch;
    }
    return result;
}

std::string compare_result_json(const CompareResult& result, std::string_view baseline) {
    std::string out = "{\"mode\":";
    append_json_string(out, to_string(result.mode));
    out += ",\"outcome\":";
    append_json_string(out, to_string(result.outcome));
    out += ",\"passed\":";
    out += result.passed() ? "true" : "false";
    out += ",\"baseline\":";
    append_json_string(out, baseline);
    out += ",\"actual\":{\"width\":" + std::to_string(result.diff.actual.width) +
           ",\"height\":" + std::to_string(result.diff.actual.height) + '}';
    out += ",\"expected\":{\"width\":" + std::to_string(result.diff.expected.width) +
           ",\"height\":" + std::to_string(result.diff.expected.height) + '}';
    out += ",\"pixels\":" + std::to_string(result.diff.pixels);
    out += ",\"mismatched\":" + std::to_string(result.diff.mismatched);
    out += ",\"maxChannelDelta\":" + std::to_string(result.diff.max_channel_delta);
    out += ",\"tolerance\":" + std::to_string(result.policy.per_channel_tolerance);
    out += ",\"maxMismatchRatio\":" + std::to_string(result.policy.max_mismatch_ratio);
    out.push_back('}');
    return out;
}

} // namespace tessera::harness
