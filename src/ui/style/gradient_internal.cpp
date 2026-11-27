// SPDX-License-Identifier: MIT

#include "gradient_internal.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace tessera::ui::style::detail {

std::vector<std::string_view> split_commas(std::string_view text) {
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size()) {
            if (text[i] == '(') {
                ++depth;
            } else if (text[i] == ')') {
                --depth;
            }
        }
        if (i == text.size() || (text[i] == ',' && depth == 0)) {
            parts.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    return parts;
}

std::vector<std::string_view> split_spaces(std::string_view text) {
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const bool end = i == text.size();
        if (!end) {
            if (text[i] == '(') {
                ++depth;
            } else if (text[i] == ')') {
                --depth;
            }
        }
        const bool at_space =
            !end && depth == 0 && std::isspace(static_cast<unsigned char>(text[i])) != 0;
        if (end || at_space) {
            if (i > start) {
                parts.push_back(text.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    return parts;
}

bool parse_number(const std::string& text, float& value, std::string& unit) {
    char* end = nullptr;
    const float parsed = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || !std::isfinite(parsed)) {
        return false;
    }
    value = parsed;
    unit = end;
    return true;
}

std::optional<float> parse_angle(std::string_view text) {
    const std::string token = lower(trim(text));
    float value = 0.0f;
    std::string unit;
    if (!parse_number(token, value, unit)) {
        return std::nullopt;
    }
    if (unit == "deg") {
        return value;
    }
    if (unit == "grad") {
        return value * 0.9f;
    }
    if (unit == "rad") {
        return value * 180.0f / 3.14159265358979f;
    }
    if (unit == "turn") {
        return value * 360.0f;
    }
    return value == 0.0f && unit.empty() ? std::optional<float>(0.0f) : std::nullopt;
}
std::optional<float> parse_percentage(std::string_view text) {
    const std::string token = lower(trim(text));
    float value = 0.0f;
    std::string unit;
    if (!parse_number(token, value, unit) || unit != "%") {
        return std::nullopt;
    }
    return value / 100.0f;
}

std::optional<float> parse_px_length(std::string_view text) {
    const std::string token = lower(trim(text));
    float value = 0.0f;
    std::string unit;
    if (!parse_number(token, value, unit)) {
        return std::nullopt;
    }
    // CSS absolute lengths at 96 dpi (CSS Values 4 section 6.2).
    static constexpr struct {
        const char* unit;
        float px;
    } kAbsolute[] = {{"px", 1.0f},          {"cm", 96.0f / 2.54f}, {"mm", 96.0f / 25.4f},
                     {"q", 96.0f / 101.6f}, {"in", 96.0f},         {"pt", 96.0f / 72.0f},
                     {"pc", 16.0f}};
    for (const auto& entry : kAbsolute) {
        if (unit == entry.unit) {
            return value * entry.px;
        }
    }
    // A bare `0` is a valid length in CSS; any other unitless number is not.
    if (unit.empty() && value == 0.0f) {
        return 0.0f;
    }
    return std::nullopt;
}

bool looks_like_stop(std::string_view text) {
    const std::vector<std::string_view> tokens = split_spaces(trim(text));
    if (tokens.empty()) {
        return false;
    }
    const std::string first = lower(tokens.front());
    return first.front() == '#' || parse_color(first).ok;
}

namespace {

// One authored position: a fraction of the line/turn, or px along the line.
struct Position {
    float value = 0.0f;
    bool length = false;
};

std::optional<Position> parse_position(std::string_view token, StopDomain domain) {
    if (const std::optional<float> fraction = parse_percentage(token)) {
        return Position{*fraction, false};
    }
    if (domain == StopDomain::Turn) {
        if (const std::optional<float> degrees = parse_angle(token)) {
            return Position{*degrees / 360.0f, false};
        }
        return std::nullopt;
    }
    if (const std::optional<float> px = parse_px_length(token)) {
        // A bare `0` is the same place in either unit; calling it a fraction keeps a list that only
        // says `0` and percentages resolvable without a box.
        return *px == 0.0f ? Position{0.0f, false} : Position{*px, true};
    }
    return std::nullopt;
}

} // namespace

bool parse_stop_list(const std::vector<std::string_view>& parts, std::size_t first,
                     StopDomain domain, std::array<GradientStopValue, kMaxGradientStops>& stops,
                     StopPositions& positions, std::size_t& count) {
    std::array<std::optional<Position>, kMaxGradientStops> authored;
    count = 0;
    for (std::size_t part = first; part < parts.size(); ++part) {
        const std::vector<std::string_view> tokens = split_spaces(parts[part]);
        // Up to two trailing position tokens; whatever precedes them is the colour. A part that is
        // only positions (a colour hint) leaves no colour and fails below.
        std::size_t colour_tokens = tokens.size();
        std::array<Position, 2> found{};
        std::size_t found_count = 0;
        while (colour_tokens > 1 && found_count < 2) {
            const std::optional<Position> position =
                parse_position(tokens[colour_tokens - 1], domain);
            if (!position) {
                break;
            }
            found[1 - found_count] = *position;
            ++found_count;
            --colour_tokens;
        }
        if (tokens.empty() || colour_tokens != 1) {
            return false;
        }
        const ColorParseResult colour = parse_color(lower(tokens[0]));
        if (!colour.ok) {
            return false;
        }
        // Two positions are two stops of one colour; none or one is a single stop.
        const std::size_t emitted = found_count == 2 ? 2 : 1;
        if (count + emitted > kMaxGradientStops) {
            return false;
        }
        for (std::size_t index = 0; index < emitted; ++index) {
            stops[count].color = colour.color;
            if (found_count != 0) {
                authored[count] = found[found_count == 2 ? index : 1];
            }
            ++count;
        }
    }
    if (count < 2) {
        return false;
    }

    positions = StopPositions{};
    for (std::size_t index = 0; index < count; ++index) {
        if (authored[index] && authored[index]->length) {
            positions.length_mask |= static_cast<std::uint8_t>(1U << index);
        }
        if (!authored[index]) {
            positions.auto_mask |= static_cast<std::uint8_t>(1U << index);
        }
    }
    if (positions.pending()) {
        for (std::size_t index = 0; index < count; ++index) {
            stops[index].offset = authored[index] ? authored[index]->value : 0.0f;
        }
        return true;
    }
    positions = StopPositions{};
    std::array<std::optional<float>, kMaxGradientStops> fractions;
    for (std::size_t index = 0; index < count; ++index) {
        if (authored[index]) {
            fractions[index] = authored[index]->value;
        }
    }
    fix_up_stop_offsets(fractions, count);
    for (std::size_t index = 0; index < count; ++index) {
        stops[index].offset = fractions[index].value();
    }
    return true;
}

void fix_up_stop_offsets(std::array<std::optional<float>, kMaxGradientStops>& positions,
                         std::size_t count) {
    if (count == 0) {
        return;
    }
    if (!positions[0]) {
        positions[0] = 0.0f;
    }
    if (!positions[count - 1]) {
        positions[count - 1] = 1.0f;
    }
    float highest = positions[0].value();
    for (std::size_t index = 1; index < count; ++index) {
        if (positions[index]) {
            positions[index] = std::max(*positions[index], highest);
            highest = *positions[index];
        }
    }
    for (std::size_t index = 1; index + 1 < count; ++index) {
        if (positions[index]) {
            continue;
        }
        std::size_t next = index + 1;
        while (!positions[next]) {
            ++next;
        }
        const float from = positions[index - 1].value();
        const float step = (*positions[next] - from) / static_cast<float>(next - index + 1);
        for (std::size_t fill = index; fill < next; ++fill) {
            positions[fill] = from + step * static_cast<float>(fill - index + 1);
        }
    }
}

} // namespace tessera::ui::style::detail
