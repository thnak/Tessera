// SPDX-License-Identifier: MIT

#include "tessera/ui/style/supports.hpp"

#include <iostream>
#include <string_view>

namespace {

using tessera::ui::style::evaluate_supports;
using tessera::ui::style::parse_supports_condition;
using tessera::ui::style::PropertyRegistry;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "supports: " << message << "\n";
    }
    return condition;
}

bool matches(std::string_view text, const PropertyRegistry& registry) {
    const auto parsed = parse_supports_condition(text);
    return parsed.ok && evaluate_supports(parsed.condition, registry);
}

} // namespace

int main() {
    const PropertyRegistry registry;

    if (!expect(matches("(display: flex)", registry), "supported property/value") ||
        !expect(!matches("(display: grid)", registry), "unsupported display value") ||
        !expect(matches("(margin: 10px)", registry), "shorthand supported") ||
        !expect(!matches("(unknown-property: 1)", registry), "unknown property") ||
        !expect(matches("(--token: 1)", registry), "custom property") ||
        !expect(matches("(position: fixed)", registry), "position value") ||
        !expect(!matches("(position: sticky-ish)", registry), "bad position value")) {
        return 1;
    }

    // Scroll runtime/style inputs (issue #248): the same grammar the declaration validator accepts,
    // so `@supports (overscroll-behavior: contain)` gates a fallback correctly.
    if (!expect(matches("(overscroll-behavior: contain)", registry),
                "overscroll-behavior contain") ||
        !expect(matches("(overscroll-behavior: auto)", registry) &&
                    matches("(overscroll-behavior: none)", registry),
                "overscroll-behavior auto/none") ||
        !expect(!matches("(overscroll-behavior: chain)", registry),
                "bad overscroll-behavior value") ||
        !expect(matches("(scroll-behavior: smooth)", registry) &&
                    matches("(scroll-behavior: auto)", registry),
                "scroll-behavior values") ||
        !expect(!matches("(scroll-behavior: instant)", registry), "bad scroll-behavior value") ||
        !expect(matches("(overscroll-effect: stretch)", registry) &&
                    matches("(overscroll-effect: glow)", registry) &&
                    matches("(overscroll-effect: bounce)", registry) &&
                    matches("(overscroll-effect: platform)", registry) &&
                    matches("(overscroll-effect: none)", registry),
                "overscroll-effect animation-v1 names") ||
        !expect(!matches("(overscroll-effect: auto)", registry), "bad overscroll-effect value") ||
        !expect(matches("(scrollbar-gutter: stable)", registry) &&
                    matches("(scrollbar-gutter: auto)", registry),
                "scrollbar-gutter values") ||
        !expect(!matches("(scrollbar-gutter: both-edges)", registry),
                "bad scrollbar-gutter value")) {
        return 5;
    }

    if (!expect(matches("not (display: grid)", registry), "not") ||
        !expect(!matches("not (display: flex)", registry), "not false") ||
        !expect(matches("(display: flex) and (color: red)", registry), "and") ||
        !expect(!matches("(display: flex) and (display: grid)", registry), "and false") ||
        !expect(matches("(display: grid) or (display: flex)", registry), "or") ||
        !expect(!matches("(display: grid) or (display: table)", registry), "or false")) {
        return 2;
    }

    if (!expect(matches("(display: grid) or (display: flex) and (display: grid)", registry) ==
                    false,
                "and binds tighter than or") ||
        !expect(matches("((display: grid) or (display: flex)) and (display: grid)", registry) ==
                    false,
                "nested condition") ||
        !expect(matches("((display: grid) or (display: flex)) and (display: block)", registry),
                "nested true condition")) {
        return 3;
    }

    if (!expect(!parse_supports_condition("").ok, "empty invalid") ||
        !expect(!parse_supports_condition("(display: flex").ok, "unterminated invalid") ||
        !expect(!parse_supports_condition("(display:)").ok, "missing value invalid") ||
        !expect(!parse_supports_condition("(display: flex) trailing").ok, "trailing invalid") ||
        !expect(!parse_supports_condition("(display: flex) and").ok, "dangling and invalid")) {
        return 4;
    }

    std::cout << "supports: ok\n";
    return 0;
}
