// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/easing.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::animation {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string trim(std::string_view value) {
    const auto first = std::find_if_not(value.begin(), value.end(),
                                        [](unsigned char c) { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
                          return std::isspace(c) != 0;
                      }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::vector<std::string> split_commas(std::string_view text) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == ',') {
            parts.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    return parts;
}

bool parse_float(std::string_view text, float& value) {
    const std::string token = trim(text);
    if (token.empty()) {
        return false;
    }
    char* end = nullptr;
    const float parsed = std::strtof(token.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

float bezier(float u, float p1, float p2) {
    const float inverse = 1.0f - u;
    return 3.0f * inverse * inverse * u * p1 + 3.0f * inverse * u * u * p2 + u * u * u;
}

float bezier_derivative(float u, float p1, float p2) {
    const float inverse = 1.0f - u;
    return 3.0f * inverse * inverse * p1 + 6.0f * inverse * u * (p2 - p1) +
           3.0f * u * u * (1.0f - p2);
}

float solve_bezier_x(float x, float x1, float x2) {
    if (x <= 0.0f) {
        return 0.0f;
    }
    if (x >= 1.0f) {
        return 1.0f;
    }

    float u = x;
    for (int i = 0; i < 8; ++i) {
        const float current = bezier(u, x1, x2);
        const float derivative = bezier_derivative(u, x1, x2);
        if (std::abs(derivative) < 1.0e-5f) {
            break;
        }
        const float next = u - (current - x) / derivative;
        if (!std::isfinite(next) || next < 0.0f || next > 1.0f) {
            break;
        }
        u = next;
        if (std::abs(current - x) < 1.0e-5f) {
            return u;
        }
    }

    float low = 0.0f;
    float high = 1.0f;
    for (int i = 0; i < 40; ++i) {
        u = (low + high) * 0.5f;
        const float current = bezier(u, x1, x2);
        if (current < x) {
            low = u;
        } else {
            high = u;
        }
    }
    return (low + high) * 0.5f;
}

Easing presets(std::string_view name) {
    if (name == "ease") {
        return Easing{EasingKind::CubicBezier, 0.25f, 0.1f, 0.25f, 1.0f};
    }
    if (name == "ease-in") {
        return Easing{EasingKind::CubicBezier, 0.42f, 0.0f, 1.0f, 1.0f};
    }
    if (name == "ease-out") {
        return Easing{EasingKind::CubicBezier, 0.0f, 0.0f, 0.58f, 1.0f};
    }
    if (name == "ease-in-out") {
        return Easing{EasingKind::CubicBezier, 0.42f, 0.0f, 0.58f, 1.0f};
    }
    return {};
}

} // namespace

EasingParseResult parse_easing(std::string_view text) {
    EasingParseResult result;
    const std::string value = lower(trim(text));
    if (value.empty() || value == "linear") {
        return result;
    }
    if (value == "ease" || value == "ease-in" || value == "ease-out" || value == "ease-in-out") {
        result.easing = presets(value);
        return result;
    }

    if (value.rfind("cubic-bezier(", 0) == 0 && value.back() == ')') {
        const std::vector<std::string> parts =
            split_commas(std::string_view(value).substr(13, value.size() - 14));
        if (parts.size() != 4) {
            result.ok = false;
            return result;
        }
        float values[4] = {};
        for (std::size_t i = 0; i < 4; ++i) {
            if (!parse_float(parts[i], values[i])) {
                result.ok = false;
                return result;
            }
        }
        if (values[0] < 0.0f || values[0] > 1.0f || values[2] < 0.0f || values[2] > 1.0f) {
            result.ok = false;
            return result;
        }
        result.easing = Easing{EasingKind::CubicBezier, values[0], values[1], values[2], values[3]};
        return result;
    }

    if (value.rfind("steps(", 0) == 0 && value.back() == ')') {
        const std::vector<std::string> parts =
            split_commas(std::string_view(value).substr(6, value.size() - 7));
        if (parts.empty() || parts.size() > 2 || parts[0].empty()) {
            result.ok = false;
            return result;
        }
        char* end = nullptr;
        const long count = std::strtol(parts[0].c_str(), &end, 10);
        if (end == nullptr || *end != '\0' || count <= 0 || count > 10000) {
            result.ok = false;
            return result;
        }
        Easing easing;
        easing.kind = EasingKind::Steps;
        easing.steps = static_cast<int>(count);
        if (parts.size() == 2) {
            const std::string position = lower(parts[1]);
            if (position == "start" || position == "jump-start") {
                easing.step_position = StepPosition::JumpStart;
            } else if (position == "end" || position == "jump-end") {
                easing.step_position = StepPosition::JumpEnd;
            } else if (position == "jump-none") {
                easing.step_position = StepPosition::JumpNone;
            } else if (position == "jump-both") {
                easing.step_position = StepPosition::JumpBoth;
            } else {
                result.ok = false;
                return result;
            }
        }
        if (easing.step_position == StepPosition::JumpNone && easing.steps < 2) {
            result.ok = false;
            return result;
        }
        result.easing = easing;
        return result;
    }

    result.ok = false;
    return result;
}

float ease(const Easing& easing, float t) {
    if (!std::isfinite(t)) {
        return 0.0f;
    }
    t = std::clamp(t, 0.0f, 1.0f);
    switch (easing.kind) {
    case EasingKind::Linear:
        return t;
    case EasingKind::CubicBezier: {
        const float u = solve_bezier_x(t, easing.x1, easing.x2);
        return bezier(u, easing.y1, easing.y2);
    }
    case EasingKind::Steps: {
        const float count = static_cast<float>(easing.steps);
        switch (easing.step_position) {
        case StepPosition::JumpEnd:
            return std::min(1.0f, std::floor(t * count) / count);
        case StepPosition::JumpStart:
            return std::min(1.0f, std::floor(t * count + 1.0f) / count);
        case StepPosition::JumpNone:
            return std::min(1.0f, std::floor(t * count) / (count - 1.0f));
        case StepPosition::JumpBoth:
            return std::min(1.0f, std::floor(t * count + 1.0f) / (count + 1.0f));
        }
        break;
    }
    }
    return t;
}

} // namespace tessera::ui::animation
