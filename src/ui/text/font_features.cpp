// SPDX-License-Identifier: MIT

#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/font_variant.hpp"
#include "tessera/ui/style/font_variant_alternates.hpp"
#include "tessera/ui/text/font_features.hpp"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace tessera::ui::text {
namespace {

using tessera::detail::lower;
using tessera::detail::trim;

constexpr std::uint32_t tag(const char (&t)[5]) {
    return font_feature_tag(t[0], t[1], t[2], t[3]);
}

bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

// Applies one font-variant group value; false (and 'out' untouched) when it is invalid. The
// keyword grammar and the feature each keyword selects live in style/font_variant.cpp.
bool apply_group(style::FontVariantGroup group, std::string_view value, FontFeatures& out) {
    const auto parsed = style::parse_font_variant_value(group, value);
    if (!parsed) {
        return false;
    }
    FontFeatures next = out;
    for (std::size_t i = 0; i < parsed->count; ++i) {
        const style::FontVariantKeyword& keyword = *parsed->items[i];
        for (std::size_t s = 0; s < keyword.count; ++s) {
            const style::FontVariantSetting& setting = keyword.settings[s];
            next.set(tag(setting.tag), setting.value);
        }
    }
    out = next;
    return true;
}

} // namespace

bool FontFeatures::set(std::uint32_t feature_tag, std::uint32_t value) noexcept {
    for (std::uint8_t i = 0; i < count; ++i) {
        if (items[i].tag == feature_tag) {
            items[i].value = value;
            return true;
        }
    }
    if (count >= kMax) {
        return false;
    }
    items[count++] = FontFeature{feature_tag, value};
    return true;
}

bool parse_font_variant_ligatures(std::string_view value, FontFeatures& out) {
    return apply_group(style::FontVariantGroup::Ligatures, value, out);
}

bool parse_font_variant_numeric(std::string_view value, FontFeatures& out) {
    return apply_group(style::FontVariantGroup::Numeric, value, out);
}

bool parse_font_variant_caps(std::string_view value, FontFeatures& out) {
    return apply_group(style::FontVariantGroup::Caps, value, out);
}

bool parse_font_variant_east_asian(std::string_view value, FontFeatures& out) {
    return apply_group(style::FontVariantGroup::EastAsian, value, out);
}

bool parse_font_variant_position(std::string_view value, FontFeatures& out) {
    return apply_group(style::FontVariantGroup::Position, value, out);
}

bool parse_font_variant_alternates(std::string_view value, FontFeatures& out) {
    const auto parsed = style::parse_font_variant_alternates(value);
    if (!parsed) {
        return false;
    }
    FontFeatures next = out;
    for (const style::FontVariantAlternateSetting& setting : *parsed) {
        next.set(font_feature_tag(setting.tag[0], setting.tag[1], setting.tag[2], setting.tag[3]),
                 setting.value);
    }
    out = next;
    return true;
}

bool parse_font_feature_settings(std::string_view value, FontFeatures& out) {
    const std::string text = std::string(trim(value));
    if (text.empty()) {
        return false;
    }
    if (lower(text) == "normal") {
        return true;
    }
    const auto skip_blank = [&](std::size_t i) {
        while (i < text.size() && is_blank(text[i])) {
            ++i;
        }
        return i;
    };
    FontFeatures next = out;
    std::size_t i = 0;
    while (true) {
        i = skip_blank(i);
        if (i >= text.size() || (text[i] != '"' && text[i] != '\'')) {
            return false;
        }
        const char quote = text[i++];
        // The tag is exactly four printable ASCII bytes (CSS Fonts 4 section 6.12).
        if (i + 5 > text.size() || text[i + 4] != quote) {
            return false;
        }
        for (std::size_t k = 0; k < 4; ++k) {
            const auto c = static_cast<unsigned char>(text[i + k]);
            if (c < 0x20 || c > 0x7E) {
                return false;
            }
        }
        const std::uint32_t feature_tag =
            font_feature_tag(text[i], text[i + 1], text[i + 2], text[i + 3]);
        i = skip_blank(i + 5);
        std::uint32_t feature_value = 1;
        const std::size_t begin = i;
        while (i < text.size() && text[i] != ',' && !is_blank(text[i])) {
            ++i;
        }
        if (i > begin) {
            const std::string token = lower(text.substr(begin, i - begin));
            if (token == "on") {
                feature_value = 1;
            } else if (token == "off") {
                feature_value = 0;
            } else {
                std::uint32_t number = 0;
                for (const char c : token) {
                    if (c < '0' || c > '9' || number > 0xFFFFU) {
                        return false;
                    }
                    number = number * 10U + static_cast<std::uint32_t>(c - '0');
                }
                feature_value = number;
            }
        }
        next.set(feature_tag, feature_value);
        i = skip_blank(i);
        if (i >= text.size()) {
            break;
        }
        if (text[i] != ',') {
            return false;
        }
        ++i;
    }
    out = next;
    return true;
}

namespace {

struct InternTable {
    std::mutex mutex;
    std::vector<FontFeatures> lists; // id - 1
};

InternTable& intern_table() {
    static InternTable table;
    return table;
}

std::atomic<std::uint64_t>& dropped_count() {
    static std::atomic<std::uint64_t> count{0};
    return count;
}

} // namespace

FontFeaturesId intern_font_features(const FontFeatures& features) {
    if (features.empty()) {
        return {};
    }
    InternTable& table = intern_table();
    const std::lock_guard lock(table.mutex);
    for (std::size_t i = 0; i < table.lists.size(); ++i) {
        if (table.lists[i] == features) {
            return {static_cast<std::uint32_t>(i + 1)};
        }
    }
    if (table.lists.size() >= kMaxInternedFontFeatures) {
        dropped_count().fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    table.lists.push_back(features);
    return {static_cast<std::uint32_t>(table.lists.size())};
}

std::uint64_t font_features_intern_dropped() noexcept {
    return dropped_count().load(std::memory_order_relaxed);
}

FontFeatures resolve_font_features(FontFeaturesId id) {
    if (id.value == 0) {
        return {};
    }
    InternTable& table = intern_table();
    const std::lock_guard lock(table.mutex);
    return id.value <= table.lists.size() ? table.lists[id.value - 1] : FontFeatures{};
}

FontFeatures parse_font_features(const FontVariantValues& values) {
    FontFeatures result;
    if (values.letter_spaced) {
        for (const char* optional : {"liga", "clig", "dlig", "hlig"}) {
            result.set(font_feature_tag(optional[0], optional[1], optional[2], optional[3]), 0);
        }
    }
    const auto apply = [&](style::FontVariantGroup group, std::string_view value) {
        if (!value.empty()) {
            apply_group(group, value, result);
        }
    };
    apply(style::FontVariantGroup::Ligatures, values.ligatures);
    apply(style::FontVariantGroup::Numeric, values.numeric);
    apply(style::FontVariantGroup::Caps, values.caps);
    apply(style::FontVariantGroup::EastAsian, values.east_asian);
    apply(style::FontVariantGroup::Position, values.position);
    if (!values.alternates.empty()) {
        parse_font_variant_alternates(values.alternates, result);
    }
    if (!values.settings.empty()) {
        parse_font_feature_settings(values.settings, result);
    }
    return result;
}

} // namespace tessera::ui::text
