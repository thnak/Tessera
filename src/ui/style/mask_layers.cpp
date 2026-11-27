// SPDX-License-Identifier: MIT

#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/mask_layers.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace tessera::ui::style {

using tessera::detail::lower;
using tessera::detail::trim;
namespace {

std::vector<std::string_view> words_of(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t start = std::string_view::npos;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const bool space = i == text.size() || tessera::detail::is_css_whitespace(text[i]);
        if (!space && start == std::string_view::npos) {
            start = i;
        } else if (space && start != std::string_view::npos) {
            words.push_back(text.substr(start, i - start));
            start = std::string_view::npos;
        }
    }
    return words;
}

// A px length, a percentage, or a bare `0`. Percent is reported as a fraction in `fraction`.
struct Measure {
    bool percent = false;
    float value = 0.0f; // px, or a fraction of the area when `percent`
};

std::optional<Measure> parse_measure(std::string_view word) {
    std::size_t end = 0;
    while (end < word.size() && (word[end] == '+' || word[end] == '-' || word[end] == '.' ||
                                 (word[end] >= '0' && word[end] <= '9'))) {
        ++end;
    }
    if (end == 0) {
        return std::nullopt;
    }
    std::string_view number = word.substr(0, end);
    if (number.front() == '+') {
        number.remove_prefix(1);
    }
    float parsed = 0.0f;
    const auto [stop, ec] = std::from_chars(number.data(), number.data() + number.size(), parsed);
    if (ec != std::errc{} || stop != number.data() + number.size() || !std::isfinite(parsed)) {
        return std::nullopt;
    }
    const std::string unit = lower(word.substr(end));
    if (unit == "px") {
        return Measure{false, parsed};
    }
    if (unit == "%") {
        return Measure{true, parsed / 100.0f};
    }
    if (unit.empty() && parsed == 0.0f) {
        return Measure{false, 0.0f};
    }
    return std::nullopt;
}

std::optional<MaskLength> parse_size_axis(std::string_view word) {
    if (lower(word) == "auto") {
        return MaskLength{};
    }
    const std::optional<Measure> measure = parse_measure(word);
    // A negative size is invalid (CSS Backgrounds 3 section 3.9).
    if (!measure || measure->value < 0.0f) {
        return std::nullopt;
    }
    return MaskLength{measure->percent ? MaskLength::Kind::Percent : MaskLength::Kind::Px,
                      measure->value};
}

enum class Edge : unsigned char { None, Left, Right, Top, Bottom, Center };

Edge edge_of(std::string_view word) {
    const std::string text = lower(word);
    if (text == "left") {
        return Edge::Left;
    }
    if (text == "right") {
        return Edge::Right;
    }
    if (text == "top") {
        return Edge::Top;
    }
    if (text == "bottom") {
        return Edge::Bottom;
    }
    return text == "center" ? Edge::Center : Edge::None;
}

// An offset from the near edge (`near`) or the far edge.
MaskPositionAxis axis_from(Edge edge, Measure offset) {
    if (edge == Edge::Center) {
        return MaskPositionAxis{0.5f, 0.0f};
    }
    const float fraction = offset.percent ? offset.value : 0.0f;
    const float px = offset.percent ? 0.0f : offset.value;
    if (edge == Edge::Right || edge == Edge::Bottom) {
        return MaskPositionAxis{1.0f - fraction, -px};
    }
    return MaskPositionAxis{fraction, px};
}

bool is_horizontal(Edge edge) {
    return edge == Edge::Left || edge == Edge::Right;
}

bool is_vertical(Edge edge) {
    return edge == Edge::Top || edge == Edge::Bottom;
}

std::optional<MaskRepeatKind> repeat_kind(std::string_view word) {
    const std::string text = lower(word);
    if (text == "repeat") {
        return MaskRepeatKind::Repeat;
    }
    if (text == "space") {
        return MaskRepeatKind::Space;
    }
    if (text == "round") {
        return MaskRepeatKind::Round;
    }
    if (text == "no-repeat") {
        return MaskRepeatKind::NoRepeat;
    }
    return std::nullopt;
}

template<typename T, typename Parse>
std::optional<MaskValueList<T>> parse_list(std::string_view text, Parse parse) {
    MaskValueList<T> list;
    for (const std::string_view entry : tessera::detail::split_commas(text)) {
        if (list.count == kMaxBackgroundLayers) {
            return std::nullopt;
        }
        const std::optional<T> value = parse(entry);
        if (!value) {
            return std::nullopt;
        }
        list.values[list.count++] = *value;
    }
    if (list.count == 0) {
        return std::nullopt;
    }
    return list;
}

} // namespace

std::optional<MaskSize> parse_mask_size(std::string_view text) {
    const std::vector<std::string_view> words = words_of(text);
    MaskSize size;
    if (words.size() == 1) {
        const std::string keyword = lower(words[0]);
        if (keyword == "cover" || keyword == "contain") {
            size.cover = keyword == "cover";
            size.contain = keyword == "contain";
            return size;
        }
    }
    if (words.empty() || words.size() > 2) {
        return std::nullopt;
    }
    const std::optional<MaskLength> width = parse_size_axis(words[0]);
    const std::optional<MaskLength> height =
        words.size() == 2 ? parse_size_axis(words[1]) : std::optional<MaskLength>(MaskLength{});
    if (!width || !height) {
        return std::nullopt;
    }
    size.width = *width;
    size.height = *height;
    return size;
}

std::optional<MaskPosition> parse_mask_position(std::string_view text) {
    const std::vector<std::string_view> words = words_of(text);
    if (words.empty() || words.size() > 4) {
        return std::nullopt;
    }
    MaskPosition position{{0.5f, 0.0f}, {0.5f, 0.0f}};
    if (words.size() <= 2) {
        // Each word is a horizontal edge, a vertical edge, `center` or a length-percentage.
        struct Slot {
            Edge edge = Edge::None;
            std::optional<Measure> measure;
        };
        std::array<Slot, 2> slots{};
        for (std::size_t i = 0; i < words.size(); ++i) {
            slots[i].edge = edge_of(words[i]);
            if (slots[i].edge == Edge::None) {
                slots[i].measure = parse_measure(words[i]);
                if (!slots[i].measure) {
                    return std::nullopt;
                }
            }
        }
        const auto is_h = [](const Slot& slot) { return is_horizontal(slot.edge); };
        const auto is_v = [](const Slot& slot) { return is_vertical(slot.edge); };
        const auto is_c = [](const Slot& slot) { return slot.edge == Edge::Center; };
        const auto apply = [](MaskPositionAxis& axis, const Slot& slot, Edge near_edge) {
            axis = slot.measure ? axis_from(near_edge, *slot.measure)
                                : axis_from(slot.edge, Measure{});
        };
        if (words.size() == 1) {
            if (is_v(slots[0])) {
                apply(position.y, slots[0], Edge::Top);
            } else if (!is_c(slots[0])) {
                apply(position.x, slots[0], Edge::Left);
            }
            return position;
        }
        // A keyword pair may come in either order (`top left`); anything with a length is x then y.
        const bool swapped = (is_v(slots[0]) && (is_h(slots[1]) || is_c(slots[1]))) ||
                             (is_c(slots[0]) && is_h(slots[1]));
        const Slot& x_slot = swapped ? slots[1] : slots[0];
        const Slot& y_slot = swapped ? slots[0] : slots[1];
        if (is_v(x_slot) || is_h(y_slot)) {
            return std::nullopt;
        }
        apply(position.x, x_slot, Edge::Left);
        apply(position.y, y_slot, Edge::Top);
        return position;
    }
    // Three or four values: each edge keyword takes an optional offset (`right 10px bottom 20px`).
    bool have_x = false;
    bool have_y = false;
    std::size_t index = 0;
    while (index < words.size()) {
        const Edge edge = edge_of(words[index]);
        if (edge == Edge::None || edge == Edge::Center) {
            return std::nullopt;
        }
        std::optional<Measure> offset;
        if (index + 1 < words.size() && edge_of(words[index + 1]) == Edge::None) {
            offset = parse_measure(words[index + 1]);
            if (!offset) {
                return std::nullopt;
            }
        }
        const MaskPositionAxis axis = axis_from(edge, offset.value_or(Measure{}));
        if (is_horizontal(edge) && !have_x) {
            position.x = axis;
            have_x = true;
        } else if (is_vertical(edge) && !have_y) {
            position.y = axis;
            have_y = true;
        } else {
            return std::nullopt;
        }
        index += offset ? 2 : 1;
    }
    // A three-value form leaves one axis unnamed, which CSS fills with `center`.
    return position;
}

std::optional<MaskRepeat> parse_mask_repeat(std::string_view text) {
    const std::vector<std::string_view> words = words_of(text);
    if (words.size() == 1) {
        const std::string keyword = lower(words[0]);
        if (keyword == "repeat-x") {
            return MaskRepeat{MaskRepeatKind::Repeat, MaskRepeatKind::NoRepeat};
        }
        if (keyword == "repeat-y") {
            return MaskRepeat{MaskRepeatKind::NoRepeat, MaskRepeatKind::Repeat};
        }
    }
    if (words.empty() || words.size() > 2) {
        return std::nullopt;
    }
    const std::optional<MaskRepeatKind> x = repeat_kind(words[0]);
    const std::optional<MaskRepeatKind> y = words.size() == 2 ? repeat_kind(words[1]) : x;
    if (!x || !y) {
        return std::nullopt;
    }
    return MaskRepeat{*x, *y};
}

std::optional<MaskMode> parse_mask_mode(std::string_view text) {
    const std::string keyword = lower(trim(text));
    if (keyword == "alpha") {
        return MaskMode::Alpha;
    }
    if (keyword == "luminance") {
        return MaskMode::Luminance;
    }
    if (keyword == "match-source") {
        return MaskMode::MatchSource;
    }
    return std::nullopt;
}

namespace {

std::optional<MaskBox> parse_box(std::string_view text, bool allow_no_clip) {
    const std::string keyword = lower(trim(text));
    if (keyword == "border-box" || keyword == "stroke-box" || keyword == "view-box") {
        return MaskBox::Border;
    }
    if (keyword == "padding-box") {
        return MaskBox::Padding;
    }
    if (keyword == "content-box" || keyword == "fill-box") {
        return MaskBox::Content;
    }
    if (allow_no_clip && keyword == "no-clip") {
        return MaskBox::NoClip;
    }
    return std::nullopt;
}

} // namespace

std::optional<MaskBox> parse_mask_origin(std::string_view text) {
    return parse_box(text, false);
}

std::optional<MaskBox> parse_mask_clip(std::string_view text) {
    return parse_box(text, true);
}

std::optional<MaskValueList<MaskSize>> parse_mask_size_list(std::string_view text) {
    return parse_list<MaskSize>(text,
                                [](std::string_view entry) { return parse_mask_size(entry); });
}

std::optional<MaskValueList<MaskPosition>> parse_mask_position_list(std::string_view text) {
    return parse_list<MaskPosition>(
        text, [](std::string_view entry) { return parse_mask_position(entry); });
}

std::optional<MaskValueList<MaskRepeat>> parse_mask_repeat_list(std::string_view text) {
    return parse_list<MaskRepeat>(text,
                                  [](std::string_view entry) { return parse_mask_repeat(entry); });
}

std::optional<MaskValueList<MaskMode>> parse_mask_mode_list(std::string_view text) {
    return parse_list<MaskMode>(text,
                                [](std::string_view entry) { return parse_mask_mode(entry); });
}

std::optional<MaskValueList<MaskBox>> parse_mask_origin_list(std::string_view text) {
    return parse_list<MaskBox>(text,
                               [](std::string_view entry) { return parse_mask_origin(entry); });
}

std::optional<MaskValueList<MaskBox>> parse_mask_clip_list(std::string_view text) {
    return parse_list<MaskBox>(text, [](std::string_view entry) { return parse_mask_clip(entry); });
}

} // namespace tessera::ui::style
