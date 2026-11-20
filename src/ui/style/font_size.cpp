// SPDX-License-Identifier: MIT

#include "tessera/ui/style/cascade.hpp"

#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>

namespace tessera::ui::style {
namespace {

constexpr float kRootFontSize = 16.0F;

// `<number><unit>`; false for a keyword or anything with a function in it.
bool split_length(std::string_view text, float& number, std::string_view& unit) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    while (!text.empty() && text.back() == ' ') {
        text.remove_suffix(1);
    }
    std::size_t end = 0;
    while (end < text.size() && ((text[end] >= '0' && text[end] <= '9') || text[end] == '.')) {
        ++end;
    }
    if (end == 0) {
        return false;
    }
    const auto parsed = std::from_chars(text.data(), text.data() + end, number);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + end) {
        return false;
    }
    unit = text.substr(end);
    return true;
}

// The parent's resolved size, or the root size when there is none (an unresolved keyword too).
float parent_font_size(const ComputedStyle* parent) {
    if (parent == nullptr) {
        return kRootFontSize;
    }
    const std::string* value = parent->find("font-size");
    float number = 0.0F;
    std::string_view unit;
    if (value != nullptr && split_length(*value, number, unit) && unit == "px") {
        return number;
    }
    return kRootFontSize;
}

} // namespace

void resolve_font_size(ComputedStyle& style, const ComputedStyle* parent) {
    const std::string* value = style.find("font-size");
    if (value == nullptr) {
        return;
    }
    float number = 0.0F;
    std::string_view unit;
    if (!split_length(*value, number, unit)) {
        return;
    }
    float pixels = 0.0F;
    if (unit.empty() || unit == "px") {
        return; // already px, or a bare number the consumers read as px
    }
    if (unit == "em") {
        pixels = number * parent_font_size(parent);
    } else if (unit == "%") {
        pixels = number * parent_font_size(parent) / 100.0F;
    } else if (unit == "rem") {
        pixels = number * kRootFontSize;
    } else if (unit == "pt") {
        pixels = number * 4.0F / 3.0F;
    } else {
        return;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%.6gpx", static_cast<double>(pixels));
    style.set("font-size", buffer);
}

} // namespace tessera::ui::style
