// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/transition.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

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

bool is_identifier(std::string_view value) {
    if (value.empty()) {
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

bool parse_times(std::string_view text, bool allow_negative, std::vector<float>& times) {
    for (const std::string& part : split_top_level(text)) {
        if (part.empty()) {
            return false;
        }
        float seconds = 0.0f;
        if (!parse_time(part, seconds) || (!allow_negative && seconds < 0.0f)) {
            return false;
        }
        times.push_back(seconds);
    }
    return true;
}

bool parse_timing_functions(std::string_view text, std::vector<Easing>& easings) {
    for (const std::string& part : split_top_level(text)) {
        if (part.empty()) {
            return false;
        }
        const EasingParseResult easing = parse_easing(part);
        if (!easing.ok) {
            return false;
        }
        easings.push_back(easing.easing);
    }
    return true;
}

bool parse_spec(std::string_view text, TransitionSpec& spec) {
    std::vector<std::string> tokens;
    if (!tokenize_spec(text, tokens) || tokens.empty()) {
        return false;
    }

    bool property_seen = false;
    bool duration_seen = false;
    bool delay_seen = false;
    bool easing_seen = false;
    for (const std::string& token : tokens) {
        if (lower(token) == "none") {
            return false;
        }

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

        const EasingParseResult easing = parse_easing(token);
        if (easing.ok) {
            if (easing_seen) {
                return false;
            }
            spec.easing = easing.easing;
            easing_seen = true;
            continue;
        }

        if (property_seen) {
            return false;
        }
        spec.property = token;
        property_seen = true;
    }
    return true;
}

} // namespace

TransitionParseResult parse_transition_list(std::string_view text) {
    TransitionParseResult result;
    const std::string trimmed = trim(text);
    if (trimmed.empty() || lower(trimmed) == "none") {
        return result;
    }

    for (const std::string& part : split_top_level(trimmed)) {
        if (part.empty()) {
            result.ok = false;
            continue;
        }
        TransitionSpec spec;
        if (!parse_spec(part, spec)) {
            result.ok = false;
            continue;
        }
        result.transitions.push_back(std::move(spec));
    }
    return result;
}

TransitionParseResult parse_transition_longhands(std::string_view properties,
                                                 std::string_view durations,
                                                 std::string_view delays,
                                                 std::string_view timing_functions) {
    TransitionParseResult result;
    const std::string property_text = trim(properties);
    if (property_text.empty()) {
        return result;
    }

    std::vector<std::string> names;
    const std::vector<std::string> property_parts = split_top_level(property_text);
    for (const std::string& part : property_parts) {
        if (part.empty() || !is_identifier(part)) {
            result.ok = false;
            return result;
        }
        if (lower(part) == "none") {
            if (property_parts.size() != 1) {
                result.ok = false;
            }
            return result;
        }
        names.push_back(part);
    }

    std::vector<float> parsed_durations;
    std::vector<float> parsed_delays;
    std::vector<Easing> parsed_easings;
    const std::string duration_text = trim(durations);
    if (!duration_text.empty() && !parse_times(duration_text, false, parsed_durations)) {
        result.ok = false;
        return result;
    }
    const std::string delay_text = trim(delays);
    if (!delay_text.empty() && !parse_times(delay_text, true, parsed_delays)) {
        result.ok = false;
        return result;
    }
    const std::string easing_text = trim(timing_functions);
    if (!easing_text.empty() && !parse_timing_functions(easing_text, parsed_easings)) {
        result.ok = false;
        return result;
    }

    const TransitionSpec defaults;
    result.transitions.reserve(names.size());
    for (std::size_t i = 0; i < names.size(); ++i) {
        TransitionSpec spec;
        spec.property = names[i];
        spec.duration =
            parsed_durations.empty() ? 0.0f : parsed_durations[i % parsed_durations.size()];
        spec.delay = parsed_delays.empty() ? 0.0f : parsed_delays[i % parsed_delays.size()];
        spec.easing =
            parsed_easings.empty() ? defaults.easing : parsed_easings[i % parsed_easings.size()];
        result.transitions.push_back(std::move(spec));
    }
    return result;
}

} // namespace tessera::ui::animation
