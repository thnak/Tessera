// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/keyframes.hpp"
#include "tessera/ui/style/color.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::animation {
namespace {

struct NumericValue {
    double value = 0.0;
    std::string unit;
    bool ok = false;
};

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

NumericValue parse_numeric(std::string_view value) {
    const std::string token = trim(value);
    if (token.empty()) {
        return {};
    }
    char* end = nullptr;
    const double number = std::strtod(token.c_str(), &end);
    if (end == nullptr || end == token.c_str() || !std::isfinite(number)) {
        return {};
    }

    const std::string unit = trim(std::string_view(end));
    for (const char c : unit) {
        if (std::isalpha(static_cast<unsigned char>(c)) == 0 && c != '%') {
            return {};
        }
    }
    return NumericValue{number, unit, true};
}

std::string format_number(double value) {
    if (std::abs(value) < 0.00005) {
        value = 0.0;
    }
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.4f", value);
    std::string result(buffer);
    while (!result.empty() && result.back() == '0') {
        result.pop_back();
    }
    if (!result.empty() && result.back() == '.') {
        result.pop_back();
    }
    return result.empty() ? "0" : result;
}

std::string interpolate_value(std::string_view from, std::string_view to, double t) {
    const style::ColorParseResult from_color = style::parse_color(from);
    const style::ColorParseResult to_color = style::parse_color(to);
    if (from_color.ok && to_color.ok) {
        return style::format_color(
            style::interpolate_color(from_color.color, to_color.color, static_cast<float>(t)));
    }

    const NumericValue lhs = parse_numeric(from);
    const NumericValue rhs = parse_numeric(to);
    if (lhs.ok && rhs.ok && lhs.unit == rhs.unit) {
        return format_number(lhs.value + (rhs.value - lhs.value) * t) + lhs.unit;
    }
    return std::string(t < 0.5 ? from : to);
}

double directed_progress(double local, double iteration, Direction direction) {
    switch (direction) {
    case Direction::Normal:
        return local;
    case Direction::Reverse:
        return 1.0 - local;
    case Direction::Alternate:
        return std::fmod(iteration, 2.0) < 1.0 ? local : 1.0 - local;
    case Direction::AlternateReverse:
        return std::fmod(iteration, 2.0) < 1.0 ? 1.0 - local : local;
    }
    return local;
}

double eased_progress(const KeyframeTiming& timing, double local, double iteration) {
    return ease(timing.easing,
                static_cast<float>(directed_progress(local, iteration, timing.direction)));
}

std::vector<style::Declaration> sample_at(const style::KeyframeBlock& keyframes, double progress) {
    if (keyframes.steps.empty()) {
        return {};
    }

    const double clamped = std::clamp(progress, 0.0, 1.0) * 100.0;
    std::size_t upper = 0;
    while (upper < keyframes.steps.size() && keyframes.steps[upper].offset < clamped) {
        ++upper;
    }

    if (upper == 0) {
        return keyframes.steps.front().declarations;
    }
    if (upper >= keyframes.steps.size()) {
        return keyframes.steps.back().declarations;
    }

    const style::KeyframeStep& from = keyframes.steps[upper - 1];
    const style::KeyframeStep& to = keyframes.steps[upper];
    const double span = to.offset - from.offset;
    const double t = span <= 0.0 ? 0.0 : (clamped - from.offset) / span;

    std::map<std::string, const style::Declaration*> from_values;
    std::map<std::string, const style::Declaration*> to_values;
    for (const style::Declaration& declaration : from.declarations) {
        from_values[declaration.property] = &declaration;
    }
    for (const style::Declaration& declaration : to.declarations) {
        to_values[declaration.property] = &declaration;
    }

    std::vector<style::Declaration> result;
    for (const auto& [property, declaration] : from_values) {
        style::Declaration sampled = *declaration;
        const auto found = to_values.find(property);
        if (found != to_values.end()) {
            sampled.value = interpolate_value(declaration->value, found->second->value, t);
        }
        result.push_back(std::move(sampled));
    }
    for (const auto& [property, declaration] : to_values) {
        if (from_values.find(property) == from_values.end()) {
            result.push_back(*declaration);
        }
    }
    return result;
}

} // namespace

std::vector<style::Declaration> sample_keyframes(const style::KeyframeBlock& keyframes,
                                                 const KeyframeTiming& timing,
                                                 double elapsed_seconds) {
    if (!std::isfinite(elapsed_seconds) || !std::isfinite(timing.delay) ||
        !std::isfinite(timing.duration) || timing.duration < 0.0 || std::isnan(timing.iterations) ||
        timing.iterations <= 0.0) {
        return {};
    }

    const bool fill_backwards = timing.fill == FillMode::Backwards || timing.fill == FillMode::Both;
    const bool fill_forwards = timing.fill == FillMode::Forwards || timing.fill == FillMode::Both;

    if (elapsed_seconds < timing.delay) {
        if (!fill_backwards) {
            return {};
        }
        return sample_at(keyframes, eased_progress(timing, 0.0, 0.0));
    }

    const double active = elapsed_seconds - timing.delay;
    if (timing.duration <= 0.0) {
        return fill_forwards ? sample_at(keyframes, eased_progress(timing, 1.0, 0.0))
                             : std::vector<style::Declaration>{};
    }

    const double total = timing.duration * timing.iterations;
    if (std::isfinite(total) && active >= total) {
        if (!fill_forwards) {
            return {};
        }
        const double whole = std::floor(timing.iterations);
        const bool exact = timing.iterations == whole;
        const double iteration = exact ? std::max(0.0, whole - 1.0) : whole;
        const double local = exact ? 1.0 : timing.iterations - whole;
        return sample_at(keyframes, eased_progress(timing, local, iteration));
    }

    const double iteration_position = active / timing.duration;
    const double iteration = std::floor(iteration_position);
    const double local = iteration_position - iteration;
    return sample_at(keyframes, eased_progress(timing, local, iteration));
}

} // namespace tessera::ui::animation
