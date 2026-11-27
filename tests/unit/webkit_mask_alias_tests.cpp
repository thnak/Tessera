// SPDX-License-Identifier: MIT
//
// The `-webkit-mask*` alias family (issue #617's last open item): the prefixed spelling must mean
// exactly what the unprefixed one does, and the two must not be able to fight in the cascade.
//
// **Why an alias and not a second implementation.** `mask-image` and its seven longhands are
// implemented once (`src/ui/style/mask_layers.cpp`, `property_mask.cpp`, the paint path in
// `paint_pass.cpp`). A browser needs the prefixed spelling because real stylesheets use it, but a
// second registered name would mean a second computed value, a second paint field and two spellings
// that can disagree - the same reason `-webkit-backdrop-filter` is folded at the registry's expand
// step (`property_registry.cpp`, `glass-v1.md` section 2) rather than registered.
//
// The one subtlety is the shorthand: `mask` has a parser of its own, so an alias that runs before
// the registry's dispatch would let `-webkit-mask: <garbage>` fall through to the generic
// single-value path and be accepted as an invalid *image*. The alias therefore maps `-webkit-mask`
// to `mask` and the shorthand is expanded from the canonical name, not from the alias.

#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/property_registry.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace style = tessera::ui::style;

int failures = 0;

void expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "webkit_mask_alias: " << message << "\n";
        ++failures;
    }
}

// The property names one declaration expands into. For an alias that is the unprefixed one's set,
// and that equality is the whole contract: a caller must not be able to tell the two spellings
// apart.
std::vector<std::string> expanded_names(std::string_view property, std::string_view value) {
    style::PropertyRegistry registry;
    const style::Declaration declaration{.property = std::string(property),
                                         .value = std::string(value),
                                         .important = false,
                                         .offset = 0};
    std::vector<std::string> names;
    for (const style::Declaration& expanded : registry.expand(declaration).declarations) {
        names.push_back(expanded.property);
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool has_diagnostic(std::string_view property, std::string_view value) {
    style::PropertyRegistry registry;
    const style::Declaration declaration{.property = std::string(property),
                                         .value = std::string(value),
                                         .important = false,
                                         .offset = 0};
    return !registry.expand(declaration).diagnostics.empty();
}

// A prefixed longhand expands to the same one canonical longhand, so the cascade sees one property.
void the_longhands_map_to_their_canonical_names() {
    const std::vector<std::pair<std::string_view, std::string_view>> cases = {
        {"-webkit-mask-image", "linear-gradient(#fff, #000)"},
        {"-webkit-mask-size", "cover"},
        {"-webkit-mask-position", "center"},
        {"-webkit-mask-repeat", "no-repeat"},
        {"-webkit-mask-mode", "alpha"},
        {"-webkit-mask-origin", "padding-box"},
        {"-webkit-mask-clip", "content-box"},
        {"-webkit-mask-composite", "add"},
    };
    for (const auto& [prefixed, value] : cases) {
        const std::vector<std::string> got = expanded_names(prefixed, value);
        const std::string canonical =
            std::string(prefixed.substr(std::string_view("-webkit-").size()));
        expect(got.size() == 1 && got.front() == canonical,
               std::string(prefixed) + " expands to exactly `" + canonical + "`");
        // And the unprefixed spelling of the same value produces the identical set.
        expect(expanded_names(canonical, value) == got,
               std::string(prefixed) + " and `" + canonical + "` expand identically");
    }
}

// The shorthand goes through the real `mask` parser, so a value the canonical spelling rejects is
// rejected through the alias too - this is the case a naive pre-dispatch alias would get wrong.
void the_shorthand_uses_the_canonical_parser() {
    const std::vector<std::string> prefixed =
        expanded_names("-webkit-mask", "linear-gradient(#fff, #000) no-repeat");
    const std::vector<std::string> canonical =
        expanded_names("mask", "linear-gradient(#fff, #000) no-repeat");
    expect(!prefixed.empty(), "-webkit-mask expands a valid layer list");
    expect(prefixed == canonical, "-webkit-mask and mask expand to the same longhands");
    // All eight longhands, so an unnamed one resets rather than inheriting a previous layer's
    // value.
    expect(prefixed.size() == 8, "-webkit-mask emits all eight longhands");

    for (const std::string_view bad : {"mask takes a nonsense value", "margin-box", ""}) {
        expect(has_diagnostic("-webkit-mask", bad) == has_diagnostic("mask", bad),
               std::string("-webkit-mask rejects `") + std::string(bad) +
                   "` exactly when mask does");
    }
}

// An unknown prefixed property is still an unknown property: the alias family is a closed list, not
// a prefix rule, so a typo keeps its diagnostic instead of silently becoming something else.
void an_unknown_prefixed_name_is_still_unknown() {
    expect(has_diagnostic("-webkit-mask-compostite", "add"),
           "-webkit-mask-compostite (a typo) is refused");
    expect(has_diagnostic("-webkit-mask-nonsense", "x"), "-webkit-mask-nonsense is refused");
}

int run() {
    the_longhands_map_to_their_canonical_names();
    the_shorthand_uses_the_canonical_parser();
    an_unknown_prefixed_name_is_still_unknown();
    if (failures == 0) {
        std::cout << "webkit_mask_alias: all checks passed\n";
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

int main() {
    return run();
}
