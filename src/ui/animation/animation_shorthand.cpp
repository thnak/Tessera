// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/animation_shorthand.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace tessera::ui::animation {
namespace {

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

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::vector<std::string> split_top_level(std::string_view text) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : '\0';
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        }
        if (i == text.size() || (c == ',' && depth <= 0)) {
            parts.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    return parts;
}

bool tokenize_spec(std::string_view text, std::vector<std::string>& tokens) {
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : '\0';
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
            if (depth < 0) {
                return false;
            }
        }
        if (i == text.size() || (std::isspace(static_cast<unsigned char>(c)) != 0 && depth == 0)) {
            std::string token = trim(text.substr(start, i - start));
            if (!token.empty()) {
                tokens.push_back(std::move(token));
            }
            start = i + 1;
        }
    }
    return depth == 0;
}

bool parse_time(std::string_view text, float& seconds) {
    std::string value = lower(text);
    float scale = 1.0f;
    if (value.size() > 2 && value.substr(value.size() - 2) == "ms") {
        value.resize(value.size() - 2);
        scale = 0.001f;
    } else if (!value.empty() && value.back() == 's') {
        value.pop_back();
    } else {
        return false;
    }
    if (value.empty()) {
        return false;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    seconds = static_cast<float>(parsed) * scale;
    return std::isfinite(seconds);
}

bool parse_iterations(std::string_view text, double& iterations) {
    const std::string value = lower(text);
    if (value == "infinite") {
        iterations = std::numeric_limits<double>::infinity();
        return true;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(parsed) || parsed < 0.0) {
        return false;
    }
    iterations = parsed;
    return true;
}

bool is_identifier(std::string_view value) {
    if (value.empty() || value == "-" || value == "--") {
        return false;
    }
    const auto is_start = [](unsigned char c) {
        return std::isalpha(c) != 0 || c == '-' || c == '_';
    };
    const auto is_part = [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_';
    };
    return is_start(static_cast<unsigned char>(value.front())) &&
           std::all_of(value.begin() + 1, value.end(),
                       [&](char c) { return is_part(static_cast<unsigned char>(c)); });
}

bool parse_spec(std::string_view text, AnimationSpec& spec) {
    std::vector<std::string> tokens;
    if (!tokenize_spec(text, tokens) || tokens.empty()) {
        return false;
    }

    bool name_seen = false;
    bool duration_seen = false;
    bool delay_seen = false;
    bool easing_seen = false;
    bool iterations_seen = false;
    bool direction_seen = false;
    bool fill_seen = false;
    bool play_seen = false;
    for (const std::string& token : tokens) {
        const std::string keyword = lower(token);

        float time = 0.0f;
        if (parse_time(token, time)) {
            if (!duration_seen) {
                if (time < 0.0f) {
                    return false;
                }
                spec.duration = time;
                duration_seen = true;
            } else if (!delay_seen) {
                spec.delay = time;
                delay_seen = true;
            } else {
                return false;
            }
            continue;
        }

        if (keyword == "infinite") {
            if (iterations_seen) {
                return false;
            }
            spec.iterations = std::numeric_limits<double>::infinity();
            iterations_seen = true;
            continue;
        }
        const char first = keyword.front();
        const bool numeric_start =
            std::isdigit(static_cast<unsigned char>(first)) != 0 || first == '.' ||
            ((first == '+' || first == '-') && keyword.size() > 1 &&
             (std::isdigit(static_cast<unsigned char>(keyword[1])) != 0 || keyword[1] == '.'));
        if (numeric_start) {
            double iterations = 0.0;
            if (!parse_iterations(keyword, iterations) || iterations_seen) {
                return false;
            }
            spec.iterations = iterations;
            iterations_seen = true;
            continue;
        }

        if (keyword == "normal" || keyword == "reverse" || keyword == "alternate" ||
            keyword == "alternate-reverse") {
            if (direction_seen) {
                return false;
            }
            if (keyword == "reverse") {
                spec.direction = Direction::Reverse;
            } else if (keyword == "alternate") {
                spec.direction = Direction::Alternate;
            } else if (keyword == "alternate-reverse") {
                spec.direction = Direction::AlternateReverse;
            } else {
                spec.direction = Direction::Normal;
            }
            direction_seen = true;
            continue;
        }

        if (keyword == "none" || keyword == "forwards" || keyword == "backwards" ||
            keyword == "both") {
            if (fill_seen) {
                return false;
            }
            if (keyword == "forwards") {
                spec.fill = FillMode::Forwards;
            } else if (keyword == "backwards") {
                spec.fill = FillMode::Backwards;
            } else if (keyword == "both") {
                spec.fill = FillMode::Both;
            } else {
                spec.fill = FillMode::None;
            }
            fill_seen = true;
            continue;
        }

        if (keyword == "running" || keyword == "paused") {
            if (play_seen) {
                return false;
            }
            spec.play_state = keyword == "paused" ? PlayState::Paused : PlayState::Running;
            play_seen = true;
            continue;
        }

        const EasingParseResult easing = parse_easing(token);
        if (easing.ok) {
            if (easing_seen) {
                return false;
            }
            spec.easing = easing.easing;
            easing_seen = true;
            continue;
        }

        if (name_seen || !is_identifier(token)) {
            return false;
        }
        spec.name = token;
        name_seen = true;
    }
    return true;
}

} // namespace

AnimationParseResult parse_animation_list(std::string_view text) {
    AnimationParseResult result;
    const std::string trimmed = trim(text);
    if (trimmed.empty()) {
        return result;
    }

    for (const std::string& part : split_top_level(trimmed)) {
        if (part.empty()) {
            result.ok = false;
            continue;
        }
        AnimationSpec spec;
        if (!parse_spec(part, spec)) {
            result.ok = false;
            continue;
        }
        result.animations.push_back(std::move(spec));
    }
    return result;
}

} // namespace tessera::ui::animation
