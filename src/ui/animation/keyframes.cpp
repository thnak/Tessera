// SPDX-License-Identifier: MIT

#include "gradient_interpolation.hpp"
#include "shadow_interpolation.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/animation/keyframes.hpp"
#include "tessera/ui/style/color.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
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
    return std::string(tessera::detail::trim(value));
}

NumericValue parse_numeric(std::string_view value) {
    // strtod needs a terminated string; a stack copy keeps this off the heap for the usual short
    // values (a heap string per sampled property per tick was an allocation each frame, #701).
    value = tessera::detail::trim(value);
    char token[64];
    if (value.empty() || value.size() >= sizeof(token)) {
        return {};
    }
    std::copy(value.begin(), value.end(), token);
    token[value.size()] = '\0';
    char* end = nullptr;
    const double number = std::strtod(token, &end);
    if (end == nullptr || end == token || !std::isfinite(number)) {
        return {};
    }

    const std::string_view unit = tessera::detail::trim(std::string_view(end));
    for (const char c : unit) {
        if (std::isalpha(static_cast<unsigned char>(c)) == 0 && c != '%') {
            return {};
        }
    }
    return NumericValue{number, std::string(unit), true};
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

// Whether `value` can be a colour: a function other than the colour ones (a `translateX(...)`) is
// not, and rejecting it here skips a parse that lowercases and copies the text twice per sampled
// property per tick (#701).
bool may_be_color(std::string_view value) {
    value = tessera::detail::trim(value);
    const std::size_t open = value.find('(');
    if (open == std::string_view::npos) {
        return true;
    }
    const std::string_view name = tessera::detail::trim(value.substr(0, open));
    constexpr std::string_view kColorFunctions[] = {"rgb",  "rgba",       "hsl",
                                                    "hsla", "light-dark", "color-mix"};
    for (const std::string_view candidate : kColorFunctions) {
        if (name.size() != candidate.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t i = 0; i < name.size() && same; ++i) {
            same = std::tolower(static_cast<unsigned char>(name[i])) == candidate[i];
        }
        if (same) {
            return true;
        }
    }
    return false;
}

std::string interpolate_value(std::string_view property, std::string_view from, std::string_view to,
                              double t) {
    if (property == "box-shadow") {
        if (std::optional<std::string> shadow = interpolate_box_shadow(from, to, t)) {
            return std::move(*shadow);
        }
    }
    if (property == "text-shadow") {
        if (std::optional<std::string> shadow = interpolate_text_shadow(from, to, t)) {
            return std::move(*shadow);
        }
    }
    if (property == "background-image") {
        if (std::optional<std::string> gradient = interpolate_gradient(from, to, t)) {
            return std::move(*gradient);
        }
    }
    if (may_be_color(from) && may_be_color(to)) {
        const style::ColorParseResult from_color = style::parse_color(from);
        const style::ColorParseResult to_color = style::parse_color(to);
        if (from_color.ok && to_color.ok) {
            return style::format_color(
                style::interpolate_color(from_color.color, to_color.color, static_cast<float>(t)));
        }
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

    // Linear scans over the (tiny) steps instead of two std::maps: the maps cost a node per
    // declaration per sampled frame (#701). Later duplicates win, as with the map assignment;
    // output keeps the map's property-sorted order.
    const auto last_of = [](const std::vector<style::Declaration>& list,
                            std::string_view property) -> const style::Declaration* {
        for (std::size_t i = list.size(); i-- > 0;) {
            if (list[i].property == property) {
                return &list[i];
            }
        }
        return nullptr;
    };

    std::vector<style::Declaration> result;
    result.reserve(from.declarations.size() + to.declarations.size());
    for (const style::Declaration& declaration : from.declarations) {
        if (last_of(from.declarations, declaration.property) != &declaration) {
            continue;
        }
        style::Declaration sampled = declaration;
        if (const style::Declaration* target = last_of(to.declarations, declaration.property)) {
            sampled.value =
                interpolate_value(declaration.property, declaration.value, target->value, t);
        }
        result.push_back(std::move(sampled));
    }
    for (const style::Declaration& declaration : to.declarations) {
        if (last_of(to.declarations, declaration.property) == &declaration &&
            last_of(from.declarations, declaration.property) == nullptr) {
            result.push_back(declaration);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const style::Declaration& l, const style::Declaration& r) {
                  return l.property < r.property;
              });
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
