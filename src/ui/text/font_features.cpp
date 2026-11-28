// SPDX-License-Identifier: MIT

#include "tessera/detail/text_util.hpp"
#include "tessera/ui/text/font_features.hpp"

#include <cstddef>
#include <iterator>
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

struct Keyword {
    std::string_view name;
    std::uint32_t tag;
    std::uint32_t value;
};

bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\n';
}

std::vector<std::string_view> words(std::string_view text) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_blank(text[i])) {
            ++i;
        }
        const std::size_t begin = i;
        while (i < text.size() && !is_blank(text[i])) {
            ++i;
        }
        if (i > begin) {
            out.push_back(text.substr(begin, i - begin));
        }
    }
    return out;
}

// Applies a space-separated keyword list; false (and 'out' untouched) on an unknown token.
bool apply_keywords(std::string_view value, const Keyword* table, std::size_t table_size,
                    FontFeatures& out) {
    const std::string text = lower(trim(value));
    if (text.empty()) {
        return false;
    }
    if (text == "normal") {
        return true;
    }
    FontFeatures next = out;
    for (const std::string_view word : words(text)) {
        bool known = false;
        for (std::size_t i = 0; i < table_size && !known; ++i) {
            if (table[i].name == word) {
                known = true;
                next.set(table[i].tag, table[i].value);
            }
        }
        if (!known) {
            return false;
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

bool parse_font_variant_numeric(std::string_view value, FontFeatures& out) {
    static const Keyword kTable[] = {
        {"lining-nums", tag("lnum"), 1},
        {"oldstyle-nums", tag("onum"), 1},
        {"proportional-nums", tag("pnum"), 1},
        {"tabular-nums", tag("tnum"), 1},
        {"diagonal-fractions", tag("frac"), 1},
        {"stacked-fractions", tag("afrc"), 1},
        {"ordinal", tag("ordn"), 1},
        {"slashed-zero", tag("zero"), 1},
    };
    return apply_keywords(value, kTable, std::size(kTable), out);
}

bool parse_font_variant_ligatures(std::string_view value, FontFeatures& out) {
    static const Keyword kTable[] = {
        {"common-ligatures", tag("liga"), 1},
        {"no-common-ligatures", tag("liga"), 0},
        {"discretionary-ligatures", tag("dlig"), 1},
        {"no-discretionary-ligatures", tag("dlig"), 0},
        {"historical-ligatures", tag("hlig"), 1},
        {"no-historical-ligatures", tag("hlig"), 0},
        {"contextual", tag("calt"), 1},
        {"no-contextual", tag("calt"), 0},
    };
    if (lower(trim(value)) == "none") {
        FontFeatures next = out;
        next.set(tag("liga"), 0);
        next.set(tag("clig"), 0);
        next.set(tag("dlig"), 0);
        next.set(tag("hlig"), 0);
        next.set(tag("calt"), 0);
        out = next;
        return true;
    }
    return apply_keywords(value, kTable, std::size(kTable), out);
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
        return {};
    }
    table.lists.push_back(features);
    return {static_cast<std::uint32_t>(table.lists.size())};
}

FontFeatures resolve_font_features(FontFeaturesId id) {
    if (id.value == 0) {
        return {};
    }
    InternTable& table = intern_table();
    const std::lock_guard lock(table.mutex);
    return id.value <= table.lists.size() ? table.lists[id.value - 1] : FontFeatures{};
}

FontFeatures parse_font_features(std::string_view ligatures, std::string_view numeric,
                                 std::string_view settings) {
    FontFeatures result;
    if (!ligatures.empty()) {
        parse_font_variant_ligatures(ligatures, result);
    }
    if (!numeric.empty()) {
        parse_font_variant_numeric(numeric, result);
    }
    if (!settings.empty()) {
        parse_font_feature_settings(settings, result);
    }
    return result;
}

} // namespace tessera::ui::text
