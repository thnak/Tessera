// SPDX-License-Identifier: MIT

#include "tessera/ui/frame_interpolation.hpp"
#include "tessera/ui/style/transform.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace tessera::ui::frame {
namespace {

constexpr float kPi = 3.14159265358979323846f;
// Below this a rotation/scale decomposition is ambiguous, so the matrix is held discrete instead.
constexpr float kScaleEpsilon = 1.0e-6f;
// Relative tolerance for the decomposition round trip. A skew (or a hand-written matrix() with one)
// has no translate/scale/rotate form; blending its entries would snap through a wrong shape.
constexpr float kDecompositionTolerance = 1.0e-4f;

constexpr const char* kBorderColorProperties[4] = {"border-top-color", "border-right-color",
                                                   "border-bottom-color", "border-left-color"};

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return text.substr(first, last - first + 1);
}

bool parse_number(std::string_view text, float& out) {
    const std::string_view trimmed = trim(text);
    if (trimmed.empty()) {
        return false;
    }
    const std::string copy(trimmed);
    errno = 0;
    char* end = nullptr;
    const float value = std::strtof(copy.c_str(), &end);
    if (end == nullptr || *end != '\0' || errno == ERANGE || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

// `currentColor` stays unresolved here: the paint pass resolves it from the style it is painting,
// which already carries the interpolated `color`, so capturing it would freeze the wrong value.
std::optional<style::ColorRgba> parse_paint_color(std::string_view text) {
    std::string lower(trim(text));
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char value) {
        return static_cast<char>(value >= 'A' && value <= 'Z' ? value - 'A' + 'a' : value);
    });
    if (lower == "currentcolor") {
        return std::nullopt;
    }
    const style::ColorParseResult parsed = style::parse_color(text);
    if (!parsed.ok) {
        return std::nullopt;
    }
    return parsed.color;
}

struct TransformParts {
    float translate_x = 0.0f;
    float translate_y = 0.0f;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float radians = 0.0f;
};

// Any affine matrix whose linear part is a rotation times a (possibly mirrored) scale reconstructs
// exactly from these parts, because the matrix translation is already the composed offset.
std::optional<TransformParts> decompose(const rendering::rhi::Mat3& matrix) {
    const float a = matrix.m[0];
    const float b = matrix.m[3];
    const float scale_x = std::hypot(a, b);
    if (!(scale_x > kScaleEpsilon) || !std::isfinite(scale_x)) {
        return std::nullopt;
    }
    const float radians = std::atan2(b, a);
    const float scale_y = (a * matrix.m[4] - b * matrix.m[1]) / scale_x;
    if (!std::isfinite(scale_y)) {
        return std::nullopt;
    }
    TransformParts parts;
    parts.translate_x = matrix.m[2];
    parts.translate_y = matrix.m[5];
    parts.scale_x = scale_x;
    parts.scale_y = scale_y;
    parts.radians = radians;

    // Validate the round trip: the off-diagonal entries must be pure rotate*scale, not a skew.
    const float cosine = std::cos(radians);
    const float sine = std::sin(radians);
    const float magnitude =
        std::max({1.0f, std::abs(scale_y), std::abs(matrix.m[1]), std::abs(matrix.m[4])});
    const float tolerance = kDecompositionTolerance * magnitude;
    if (std::abs(matrix.m[1] - (-scale_y * sine)) > tolerance ||
        std::abs(matrix.m[4] - (scale_y * cosine)) > tolerance) {
        return std::nullopt;
    }
    // Perspective terms this 2D representation cannot express (and never produces).
    if (std::abs(matrix.m[6]) > kDecompositionTolerance ||
        std::abs(matrix.m[7]) > kDecompositionTolerance) {
        return std::nullopt;
    }
    return parts;
}

rendering::rhi::Mat3 compose(const TransformParts& parts) {
    const float cosine = std::cos(parts.radians);
    const float sine = std::sin(parts.radians);
    rendering::rhi::Mat3 matrix;
    matrix.m[0] = parts.scale_x * cosine;
    matrix.m[1] = -parts.scale_y * sine;
    matrix.m[2] = parts.translate_x;
    matrix.m[3] = parts.scale_x * sine;
    matrix.m[4] = parts.scale_y * cosine;
    matrix.m[5] = parts.translate_y;
    return matrix;
}

// Shorter arc, so a rotation from 170deg to -170deg blends through 180deg instead of spinning back
// almost a full turn. Deterministic: the same pair always picks the same direction.
float wrap_radians(float angle) {
    while (angle > kPi) {
        angle -= 2.0f * kPi;
    }
    while (angle <= -kPi) {
        angle += 2.0f * kPi;
    }
    return angle;
}

rendering::rhi::Mat3 interpolate_matrix(const rendering::rhi::Mat3& from,
                                        const rendering::rhi::Mat3& to, float alpha) {
    if (alpha <= 0.0f) {
        return from;
    }
    if (alpha >= 1.0f) {
        return to;
    }
    const std::optional<TransformParts> previous = decompose(from);
    const std::optional<TransformParts> current = decompose(to);
    if (!previous || !current) {
        // Unrepresentable (skewed) transforms hold the current tick value rather than wobble.
        return to;
    }
    TransformParts blended;
    blended.translate_x =
        previous->translate_x + (current->translate_x - previous->translate_x) * alpha;
    blended.translate_y =
        previous->translate_y + (current->translate_y - previous->translate_y) * alpha;
    blended.scale_x = previous->scale_x + (current->scale_x - previous->scale_x) * alpha;
    blended.scale_y = previous->scale_y + (current->scale_y - previous->scale_y) * alpha;
    blended.radians =
        previous->radians + wrap_radians(current->radians - previous->radians) * alpha;
    return compose(blended);
}

float interpolate_number(float from, float to, float alpha) {
    if (alpha <= 0.0f) {
        return from;
    }
    if (alpha >= 1.0f) {
        return to;
    }
    return from + (to - from) * alpha;
}

std::optional<style::ColorRgba> interpolate_color(const std::optional<style::ColorRgba>& from,
                                                  const std::optional<style::ColorRgba>& to,
                                                  float alpha) {
    if (!from || !to) {
        return to;
    }
    if (alpha <= 0.0f) {
        return from;
    }
    if (alpha >= 1.0f) {
        return to;
    }
    return style::interpolate_color(*from, *to, alpha);
}

bool is_empty(const PaintSample& sample) {
    if (sample.transform || sample.opacity || sample.color || sample.background_color) {
        return false;
    }
    for (const auto& border : sample.border_colors) {
        if (border) {
            return false;
        }
    }
    return true;
}

} // namespace

void capture_paint_sample(PaintSamples& samples, const Node& scene_node,
                          const std::vector<style::Declaration>& declarations) {
    if (declarations.empty()) {
        return;
    }
    // `transform-origin` may come after `transform` in the same list; resolve the whole tick's
    // origin first so the matrix does not depend on declaration order.
    std::string_view origin = "50% 50%";
    for (const style::Declaration& declaration : declarations) {
        if (declaration.property == "transform-origin") {
            origin = declaration.value;
        }
    }

    PaintSample captured;
    for (const style::Declaration& declaration : declarations) {
        const std::string& property = declaration.property;
        if (property == "transform") {
            const std::optional<rendering::rhi::Mat3> matrix = style::compute_transform(
                declaration.value, origin, scene_node.bounds().width, scene_node.bounds().height);
            if (matrix) {
                captured.transform = *matrix;
            }
        } else if (property == "opacity") {
            float value = 1.0f;
            if (parse_number(declaration.value, value)) {
                captured.opacity = std::clamp(value, 0.0f, 1.0f);
            }
        } else if (property == "color") {
            if (std::optional<style::ColorRgba> color = parse_paint_color(declaration.value)) {
                captured.color = *color;
            }
        } else if (property == "background-color") {
            if (std::optional<style::ColorRgba> color = parse_paint_color(declaration.value)) {
                captured.background_color = *color;
            }
        } else {
            for (int index = 0; index < 4; ++index) {
                if (property == kBorderColorProperties[index]) {
                    if (std::optional<style::ColorRgba> color =
                            parse_paint_color(declaration.value)) {
                        captured.border_colors[static_cast<std::size_t>(index)] = *color;
                    }
                    break;
                }
            }
        }
    }
    // Layout- and text-feeding declarations leave no entry, so a width/font animation is provably
    // invisible to the interpolation view.
    if (!is_empty(captured)) {
        samples[scene_node.id()] = captured;
    }
}

PaintSamples interpolate_paint_samples(const PaintSamples& previous, const PaintSamples& current,
                                       double alpha) {
    const float fraction =
        static_cast<float>(std::clamp(std::isfinite(alpha) ? alpha : 0.0, 0.0, 1.0));
    PaintSamples result;
    for (const auto& [node, target] : current) {
        const PaintSample* source = nullptr;
        if (const auto found = previous.find(node); found != previous.end()) {
            source = &found->second;
        }
        PaintSample blended;
        // A field the current tick no longer sets is dropped, not held: the caller's style provider
        // owns the reverted value, and holding the last animated one would override it forever.
        if (source != nullptr && source->transform && target.transform) {
            blended.transform = interpolate_matrix(*source->transform, *target.transform, fraction);
        } else {
            blended.transform = target.transform;
        }
        if (source != nullptr && source->opacity && target.opacity) {
            blended.opacity = interpolate_number(*source->opacity, *target.opacity, fraction);
        } else {
            blended.opacity = target.opacity;
        }
        blended.color =
            interpolate_color(source ? source->color : std::nullopt, target.color, fraction);
        blended.background_color = interpolate_color(
            source ? source->background_color : std::nullopt, target.background_color, fraction);
        for (std::size_t index = 0; index < blended.border_colors.size(); ++index) {
            blended.border_colors[index] =
                interpolate_color(source ? source->border_colors[index] : std::nullopt,
                                  target.border_colors[index], fraction);
        }
        result[node] = blended;
    }
    return result;
}

} // namespace tessera::ui::frame
