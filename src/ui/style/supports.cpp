// SPDX-License-Identifier: MIT

#include "tessera/ui/style/backdrop_filter.hpp"
#include "tessera/ui/style/interaction_values.hpp"
#include "tessera/ui/style/supports.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

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

bool is_word_char(unsigned char c) {
    return std::isalnum(c) != 0 || c == '-' || c == '_';
}

bool keyword_in(std::string_view value, const std::vector<std::string_view>& allowed) {
    const std::string normalized = lower(trim(value));
    return std::find(allowed.begin(), allowed.end(), normalized) != allowed.end();
}

bool value_supported(std::string_view property, std::string_view value) {
    const std::string normalized = lower(trim(value));
    if (normalized.empty()) {
        return false;
    }
    if (normalized == "inherit" || normalized == "initial" || normalized == "unset" ||
        normalized == "revert" || normalized == "revert-layer" ||
        normalized.find("var(") != std::string::npos) {
        return true;
    }

    if (property == "display") {
        return keyword_in(normalized, {"block", "inline", "inline-block", "flex", "inline-flex",
                                       "none", "contents"});
    }
    if (property == "position") {
        return keyword_in(normalized, {"static", "relative", "absolute", "fixed", "sticky"});
    }
    if (property == "flex-direction") {
        return keyword_in(normalized, {"row", "row-reverse", "column", "column-reverse"});
    }
    if (property == "flex-wrap") {
        return keyword_in(normalized, {"nowrap", "wrap", "wrap-reverse"});
    }
    if (property == "overflow" || property == "overflow-x" || property == "overflow-y") {
        return keyword_in(normalized, {"visible", "hidden", "scroll", "auto", "clip"});
    }
    // Scroll runtime/style inputs (stylesheet-v1 section 11). `@supports` is exactly as strict as
    // the declaration validator, so a stylesheet can gate its fallback on what the engine accepts.
    if (property == "scroll-behavior") {
        return keyword_in(normalized, {"auto", "smooth"});
    }
    if (property == "overscroll-behavior") {
        return keyword_in(normalized, {"auto", "contain", "none"});
    }
    if (property == "overscroll-effect") {
        return keyword_in(normalized, {"none", "glow", "stretch", "bounce", "platform"});
    }
    if (property == "scrollbar-gutter") {
        return keyword_in(normalized, {"auto", "stable"});
    }
    if (property == "box-sizing") {
        return keyword_in(normalized, {"content-box", "border-box"});
    }
    // Glass (glass-v1 section 2): `@supports (backdrop-filter: blur(1px))` is true wherever glass
    // is implemented, and `@supports` is exactly as strict as the declaration validator, so a
    // stylesheet can gate its fallback on the same grammar the engine accepts.
    if (property == "backdrop-filter" || property == "-webkit-backdrop-filter") {
        return parse_backdrop_filter(normalized).ok();
    }
    if (property == "font-style") {
        return keyword_in(normalized, {"normal", "italic", "oblique"});
    }
    if (property == "white-space") {
        return keyword_in(normalized,
                          {"normal", "nowrap", "pre", "pre-wrap", "pre-line", "break-spaces"});
    }
    if (property == "text-align") {
        return keyword_in(normalized, {"start", "end", "left", "right", "center", "justify"});
    }
    if (property == "text-transform") {
        return keyword_in(normalized, {"none", "capitalize", "uppercase", "lowercase"});
    }
    if (property == "text-overflow") {
        // One value only: the two-value `<overflow> <side>` form is not implemented (the registry
        // rejects it too, docs/design/text.md section 6.3).
        return keyword_in(normalized, {"clip", "ellipsis"});
    }
    if (property == "visibility") {
        return keyword_in(normalized, {"visible", "hidden", "collapse"});
    }
    if (property == "direction") {
        return keyword_in(normalized, {"ltr", "rtl"});
    }
    if (property == "writing-mode") {
        return keyword_in(normalized, {"horizontal-tb", "vertical-rl", "vertical-lr"});
    }
    // Interaction properties (issue #247, stylesheet-v1 section 11): `@supports` is exactly as
    // strict as the declaration validator, so a stylesheet can gate its fallback on the same
    // grammar.
    if (is_interaction_value_property(property)) {
        return is_supported_interaction_value(property, normalized);
    }
    return true;
}

class SupportsParser {
public:
    explicit SupportsParser(std::string_view text)
        : text_(text) {}

    SupportsParseResult parse() {
        SupportsParseResult result;
        result.condition = parse_or();
        skip_space();
        if (!ok_ || position_ != text_.size()) {
            result.ok = false;
            result.error_offset = position_;
        }
        return result;
    }

private:
    void skip_space() {
        while (position_ < text_.size() &&
               std::isspace(static_cast<unsigned char>(text_[position_])) != 0) {
            ++position_;
        }
    }

    bool consume_word(std::string_view word) {
        skip_space();
        if (position_ + word.size() > text_.size()) {
            return false;
        }
        for (std::size_t i = 0; i < word.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(text_[position_ + i])) != word[i]) {
                return false;
            }
        }
        const std::size_t end = position_ + word.size();
        if (end < text_.size() && is_word_char(static_cast<unsigned char>(text_[end])) != 0) {
            return false;
        }
        position_ = end;
        return true;
    }

    std::size_t matching_paren(std::size_t open) const {
        int depth = 0;
        char quote = 0;
        for (std::size_t i = open; i < text_.size(); ++i) {
            const char c = text_[i];
            if (quote != 0) {
                if (c == '\\' && i + 1 < text_.size()) {
                    ++i;
                } else if (c == quote) {
                    quote = 0;
                }
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
                if (depth == 0) {
                    return i;
                }
            }
        }
        return std::string_view::npos;
    }

    std::size_t top_level_colon(std::string_view text) const {
        int depth = 0;
        char quote = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const char c = text[i];
            if (quote != 0) {
                if (c == '\\' && i + 1 < text.size()) {
                    ++i;
                } else if (c == quote) {
                    quote = 0;
                }
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '(') {
                ++depth;
            } else if (c == ')') {
                --depth;
            } else if (c == ':' && depth == 0) {
                return i;
            }
        }
        return std::string_view::npos;
    }

    SupportsCondition parse_or() {
        std::vector<SupportsCondition> children;
        children.push_back(parse_and());
        while (consume_word("or")) {
            children.push_back(parse_and());
        }
        if (children.size() == 1) {
            return std::move(children.front());
        }
        SupportsCondition condition;
        condition.kind = SupportsKind::Or;
        condition.children = std::move(children);
        return condition;
    }

    SupportsCondition parse_and() {
        std::vector<SupportsCondition> children;
        children.push_back(parse_unary());
        while (consume_word("and")) {
            children.push_back(parse_unary());
        }
        if (children.size() == 1) {
            return std::move(children.front());
        }
        SupportsCondition condition;
        condition.kind = SupportsKind::And;
        condition.children = std::move(children);
        return condition;
    }

    SupportsCondition parse_unary() {
        skip_space();
        if (consume_word("not")) {
            SupportsCondition condition;
            condition.kind = SupportsKind::Not;
            condition.children.push_back(parse_unary());
            return condition;
        }
        return parse_primary();
    }

    SupportsCondition parse_primary() {
        skip_space();
        if (position_ >= text_.size() || text_[position_] != '(') {
            ok_ = false;
            return {};
        }
        const std::size_t close = matching_paren(position_);
        if (close == std::string_view::npos) {
            ok_ = false;
            return {};
        }

        const std::string_view content = text_.substr(position_ + 1, close - position_ - 1);
        position_ = close + 1;
        const std::size_t colon = top_level_colon(content);
        if (colon != std::string_view::npos) {
            SupportsCondition condition;
            condition.kind = SupportsKind::Declaration;
            condition.property = trim(content.substr(0, colon));
            condition.value = trim(content.substr(colon + 1));
            if (condition.property.empty() || condition.value.empty()) {
                ok_ = false;
            }
            return condition;
        }

        SupportsParser nested(content);
        SupportsParseResult parsed = nested.parse();
        if (!parsed.ok) {
            ok_ = false;
            return {};
        }
        return std::move(parsed.condition);
    }

    std::string_view text_;
    std::size_t position_ = 0;
    bool ok_ = true;
};

} // namespace

SupportsParseResult parse_supports_condition(std::string_view text) {
    SupportsParser parser(text);
    return parser.parse();
}

bool evaluate_supports(const SupportsCondition& condition, const PropertyRegistry& registry) {
    switch (condition.kind) {
    case SupportsKind::Declaration: {
        const std::string property = lower(trim(condition.property));
        return registry.supported(property) && value_supported(property, condition.value);
    }
    case SupportsKind::Not:
        return condition.children.empty()
                   ? false
                   : !evaluate_supports(condition.children.front(), registry);
    case SupportsKind::And:
        for (const SupportsCondition& child : condition.children) {
            if (!evaluate_supports(child, registry)) {
                return false;
            }
        }
        return !condition.children.empty();
    case SupportsKind::Or:
        for (const SupportsCondition& child : condition.children) {
            if (evaluate_supports(child, registry)) {
                return true;
            }
        }
        return false;
    }
    return false;
}

} // namespace tessera::ui::style
