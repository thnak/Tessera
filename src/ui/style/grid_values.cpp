// SPDX-License-Identifier: MIT
//
// The CSS Grid value grammar shared by the declaration validator, `@supports` and the layout
// engine (docs/specs/grid-v1.md). Keeping one parser here means a value the engine accepts can
// never disagree with what `@supports` reports.

#include "tessera/ui/style/grid_values.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace tessera::ui::style {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return text.substr(first, last - first + 1);
}

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool parse_float(std::string_view text, float& value) {
    const std::string_view token = trim(text);
    if (token.empty()) {
        return false;
    }
    const char* first = token.data();
    const char* last = token.data() + token.size();
    const auto result = std::from_chars(first, last, value, std::chars_format::general);
    return result.ec == std::errc{} && result.ptr == last && std::isfinite(value);
}

// Splits on `separator` at parenthesis depth 0. Quotes are not part of the grammar these values
// use, so only parentheses are tracked.
std::vector<std::string_view> split_top_level(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth > 0) {
                --depth;
            }
        } else if (c == separator && depth == 0) {
            parts.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    parts.push_back(text.substr(start));
    return parts;
}

// Splits on top-level whitespace, keeping a `repeat(...)`/`minmax(...)` call in one token.
std::vector<std::string_view> split_tokens(std::string_view text) {
    std::vector<std::string_view> tokens;
    std::size_t start = std::string_view::npos;
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            if (depth > 0) {
                --depth;
            }
        }
        const bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (space && depth == 0) {
            if (start != std::string_view::npos) {
                tokens.push_back(text.substr(start, i - start));
                start = std::string_view::npos;
            }
            continue;
        }
        if (start == std::string_view::npos) {
            start = i;
        }
    }
    if (start != std::string_view::npos) {
        tokens.push_back(text.substr(start));
    }
    return tokens;
}

// `<length-percentage>`: the unit set is the stylesheet engine's (px, rem/em, vw, vh, pt, %).
bool parse_length_percentage(std::string_view token, GridTrackSize& out) {
    const std::string_view text = trim(token);
    if (text.empty()) {
        return false;
    }
    if (text.back() == '%') {
        float value = 0.0f;
        if (!parse_float(text.substr(0, text.size() - 1), value) || value < 0.0f) {
            return false;
        }
        out = GridTrackSize{GridTrackKind::Percent, value, GridLengthUnit::None};
        return true;
    }
    std::size_t unit_start = 0;
    while (unit_start < text.size() &&
           (std::isdigit(static_cast<unsigned char>(text[unit_start])) != 0 ||
            text[unit_start] == '.' || text[unit_start] == '+' || text[unit_start] == '-')) {
        ++unit_start;
    }
    const std::string_view unit(text.data() + unit_start, text.size() - unit_start);
    float value = 0.0f;
    if (!parse_float(text.substr(0, unit_start), value) || value < 0.0f) {
        return false;
    }
    GridLengthUnit parsed_unit = GridLengthUnit::None;
    if (unit.empty() || unit == "px") {
        parsed_unit = GridLengthUnit::Px;
    } else if (unit == "rem") {
        parsed_unit = GridLengthUnit::Rem;
    } else if (unit == "em") {
        parsed_unit = GridLengthUnit::Em;
    } else if (unit == "vw") {
        parsed_unit = GridLengthUnit::Vw;
    } else if (unit == "vh") {
        parsed_unit = GridLengthUnit::Vh;
    } else if (unit == "pt") {
        parsed_unit = GridLengthUnit::Pt;
    } else {
        return false;
    }
    out = GridTrackSize{GridTrackKind::Fixed, value, parsed_unit};
    return true;
}

bool parse_breadth(std::string_view token, bool allow_flex, GridTrackSize& out) {
    const std::string text = lower(trim(token));
    if (text.empty()) {
        return false;
    }
    if (text == "auto") {
        out = GridTrackSize{GridTrackKind::Auto, 0.0f, GridLengthUnit::None};
        return true;
    }
    if (text == "min-content") {
        out = GridTrackSize{GridTrackKind::MinContent, 0.0f, GridLengthUnit::None};
        return true;
    }
    if (text == "max-content") {
        out = GridTrackSize{GridTrackKind::MaxContent, 0.0f, GridLengthUnit::None};
        return true;
    }
    if (text.size() > 2 && text.compare(text.size() - 2, 2, "fr") == 0) {
        if (!allow_flex) {
            return false;
        }
        float value = 0.0f;
        if (!parse_float(std::string_view(text).substr(0, text.size() - 2), value) ||
            value < 0.0f) {
            return false;
        }
        out = GridTrackSize{GridTrackKind::Fr, value, GridLengthUnit::None};
        return true;
    }
    return parse_length_percentage(text, out);
}

bool parse_track(std::string_view token, GridTrack& out) {
    const std::string text = lower(trim(token));
    if (text.empty()) {
        return false;
    }
    if (text.size() > 7 && text.compare(0, 7, "minmax(") == 0 && text.back() == ')') {
        const std::string_view inner(text.data() + 7, text.size() - 8);
        const std::vector<std::string_view> parts = split_top_level(inner, ',');
        if (parts.size() != 2) {
            return false;
        }
        GridTrackSize minimum;
        GridTrackSize maximum;
        if (!parse_breadth(parts[0], false, minimum) || !parse_breadth(parts[1], true, maximum)) {
            return false;
        }
        out = GridTrack{minimum, maximum};
        return true;
    }
    GridTrackSize size;
    if (!parse_breadth(text, true, size)) {
        return false;
    }
    if (size.kind == GridTrackKind::Fr) {
        out = GridTrack{GridTrackSize{GridTrackKind::Auto, 0.0f, GridLengthUnit::None}, size};
    } else {
        out = GridTrack{size, size};
    }
    return true;
}

constexpr std::size_t kMaxTracks = 1000;

bool parse_repeat(std::string_view token, std::vector<GridTrack>& tracks) {
    const std::string_view inner(token.data() + 7, token.size() - 8);
    const std::vector<std::string_view> parts = split_top_level(inner, ',');
    if (parts.size() != 2) {
        return false;
    }
    float count = 0.0f;
    if (!parse_float(parts[0], count) || count < 1.0f || count != std::floor(count) ||
        count > static_cast<float>(kMaxTracks)) {
        return false;
    }
    const std::vector<std::string_view> inner_tokens = split_tokens(parts[1]);
    if (inner_tokens.empty()) {
        return false;
    }
    std::vector<GridTrack> repeated;
    repeated.reserve(inner_tokens.size());
    for (const std::string_view inner_token : inner_tokens) {
        // `repeat()` cannot nest in CSS; keeping it flat also bounds the expansion.
        if (lower(trim(inner_token)).compare(0, 7, "repeat(") == 0) {
            return false;
        }
        GridTrack track;
        if (!parse_track(inner_token, track)) {
            return false;
        }
        repeated.push_back(track);
    }
    const auto repeat_count = static_cast<std::size_t>(count);
    if (tracks.size() + repeated.size() * repeat_count > kMaxTracks) {
        return false;
    }
    for (std::size_t i = 0; i < repeat_count; ++i) {
        tracks.insert(tracks.end(), repeated.begin(), repeated.end());
    }
    return true;
}

// One side of a `grid-column`/`grid-row` value: a line number or `span <n>`.
struct LineSide {
    int line = 0;
    int span = 1;
    bool is_span = false;
};

bool parse_line_side(std::string_view text, LineSide& out) {
    const std::vector<std::string_view> tokens = split_tokens(trim(text));
    if (tokens.empty() || tokens.size() > 2) {
        return false;
    }
    if (tokens.size() == 1) {
        const std::string token = lower(trim(tokens[0]));
        if (token == "auto") {
            out = LineSide{};
            return true;
        }
        float number = 0.0f;
        if (!parse_float(token, number) || number != std::floor(number) || number == 0.0f ||
            std::abs(number) > 100000.0f) {
            return false;
        }
        out = LineSide{static_cast<int>(number), 1, false};
        return true;
    }
    if (lower(trim(tokens[0])) != "span") {
        return false;
    }
    float number = 0.0f;
    if (!parse_float(tokens[1], number) || number != std::floor(number) || number < 1.0f ||
        number > 100000.0f) {
        return false;
    }
    out = LineSide{0, static_cast<int>(number), true};
    return true;
}

bool alignment_keyword(std::string_view value, bool allow_auto) {
    const std::string token = lower(trim(value));
    return token == "start" || token == "end" || token == "center" || token == "stretch" ||
           token == "normal" || token == "left" || token == "right" || token == "flex-start" ||
           token == "flex-end" || token == "legacy" || token == "baseline" ||
           (allow_auto && token == "auto");
}

} // namespace

GridTemplate parse_grid_template(std::string_view value) {
    GridTemplate result;
    const std::string text = lower(trim(value));
    if (text.empty()) {
        return result;
    }
    if (text == "none") {
        result.valid = true;
        return result;
    }
    const std::vector<std::string_view> tokens = split_tokens(text);
    for (const std::string_view token : tokens) {
        if (token.size() > 8 && token.compare(0, 7, "repeat(") == 0 && token.back() == ')') {
            if (!parse_repeat(token, result.tracks)) {
                return GridTemplate{};
            }
            continue;
        }
        GridTrack track;
        if (!parse_track(token, track)) {
            return GridTemplate{};
        }
        if (result.tracks.size() >= kMaxTracks) {
            return GridTemplate{};
        }
        result.tracks.push_back(track);
    }
    result.valid = !result.tracks.empty();
    return result;
}

GridLine parse_grid_line(std::string_view value) {
    GridLine result;
    const std::string text = lower(trim(value));
    if (text.empty()) {
        return result;
    }
    const std::vector<std::string_view> parts = split_top_level(text, '/');
    if (parts.empty() || parts.size() > 2) {
        return result;
    }
    LineSide start;
    if (!parse_line_side(parts[0], start)) {
        return result;
    }
    if (parts.size() == 1) {
        result.start = start.line;
        result.end = 0;
        result.span = start.span;
        result.valid = true;
        return result;
    }
    LineSide end;
    if (!parse_line_side(parts[1], end)) {
        return result;
    }
    if (start.is_span && end.is_span) {
        return result;
    }
    if (start.is_span) {
        // `span 2 / 4` ends at line 4 and starts two tracks earlier.
        result.start = 0;
        result.end = end.line;
        result.span = start.span;
    } else if (end.is_span) {
        result.start = start.line;
        result.end = 0;
        result.span = end.span;
    } else {
        result.start = start.line;
        result.end = end.line;
        result.span = 1;
    }
    result.valid = true;
    return result;
}

bool parse_grid_length(std::string_view value, GridTrackSize& out) {
    GridTrackSize size;
    if (!parse_length_percentage(value, size)) {
        return false;
    }
    out = size;
    return true;
}

bool is_supported_grid_auto_flow(std::string_view value) {
    const std::string token = lower(trim(value));
    return token == "row" || token == "column";
}

bool is_supported_grid_alignment(std::string_view value) {
    return alignment_keyword(value, /*allow_auto=*/true);
}

} // namespace tessera::ui::style
