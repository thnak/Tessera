// SPDX-License-Identifier: MIT

// The theme source build (spike #662, ADR-064 item 6): a resolved chain of theme sources in, plain
// CSS custom properties inside `@layer` out.
//
// The output is deliberately ordinary CSS. ADR-064 item 6 protects three properties, and each is a
// property of this file's output rather than of a runtime:
//   * one token name per role, whose value is a `light-dark()` pair, so a theme has no separate
//     light and dark names;
//   * everything inside `@layer`, so unlayered app CSS outranks it regardless of specificity; and
//   * no theme runtime: the app loads the CSS (or the theme asset blob) exactly as it does today.
//
// The build also reports the vocabulary cost the spike measures: how many tokens the theme defines
// against how many the components actually read.

#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/theme_source.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

// A declaration value that reads `var(--name)` contributes `--name` to the read set. The scan is
// textual because the value is CSS text at this point; it is the same shape `var_resolve.cpp`
// parses at computed-value time, and the spike only needs the names, not the fallbacks.
void collect_read_tokens(std::string_view value, std::vector<std::string>& out) {
    constexpr std::string_view kVar = "var(";
    std::size_t index = 0;
    while ((index = value.find(kVar, index)) != std::string_view::npos) {
        std::size_t start = index + kVar.size();
        while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
            ++start;
        }
        std::size_t end = start;
        while (end < value.size() && value[end] != ',' && value[end] != ')' && value[end] != ' ' &&
               value[end] != '\t') {
            ++end;
        }
        const std::string_view name = value.substr(start, end - start);
        if (name.size() > 2 && name.starts_with("--")) {
            const std::string owned(name);
            if (std::find(out.begin(), out.end(), owned) == out.end()) {
                out.push_back(owned);
            }
        }
        index = end;
    }
}

// True when `value` is a colour, so it takes a `light-dark()` pair. A length, a font stack or a
// number does not: `light-dark()` is a colour function and the engine rejects it elsewhere
// (`color_functions.cpp`), so emitting one for a length would produce CSS the engine drops.
bool is_colour_value(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    if (value.front() == '#') {
        return true;
    }
    constexpr std::string_view kFunctions[] = {"rgb(",        "rgba(",      "hsl(",   "hsla(",
                                               "light-dark(", "color-mix(", "oklch(", "oklab(",
                                               "lab(",        "lch("};
    for (const std::string_view function : kFunctions) {
        if (value.size() >= function.size() &&
            std::equal(function.begin(), function.end(), value.begin(), [](char a, char b) {
                return a == (b >= 'A' && b <= 'Z' ? static_cast<char>(b - 'A' + 'a') : b);
            })) {
            return true;
        }
    }
    // A named colour: the engine's own table decides, so the build cannot disagree with the paint
    // path about whether `rebeccapurple` is a colour.
    return parse_color(value).ok;
}

// One resolved theme: the flat token list and the component keys, later entries already applied.
struct Resolved {
    std::string name;
    std::vector<ThemeSourceToken> tokens;
    std::vector<ThemeSourceComponent> components;
};

void merge_into(Resolved& into, const ThemeSource& source) {
    into.name = source.name;
    for (const ThemeSourceToken& token : source.tokens) {
        const auto found = std::find_if(
            into.tokens.begin(), into.tokens.end(),
            [&token](const ThemeSourceToken& existing) { return existing.name == token.name; });
        if (found == into.tokens.end()) {
            into.tokens.push_back(token);
        } else {
            *found = token;
        }
    }
    for (const ThemeSourceComponent& component : source.components) {
        const auto found = std::find_if(into.components.begin(), into.components.end(),
                                        [&component](const ThemeSourceComponent& existing) {
                                            return existing.name == component.name;
                                        });
        if (found == into.components.end()) {
            into.components.push_back(component);
            continue;
        }
        // A derived theme overrides per key and per declaration, so it can restate one property of
        // one key without repeating the rest.
        for (const ThemeSourceKey& key : component.keys) {
            const auto existing_key = std::find_if(
                found->keys.begin(), found->keys.end(),
                [&key](const ThemeSourceKey& candidate) { return candidate.key == key.key; });
            if (existing_key == found->keys.end()) {
                found->keys.push_back(key);
                continue;
            }
            for (const ThemeToken& declaration : key.declarations) {
                const auto existing_declaration = std::find_if(
                    existing_key->declarations.begin(), existing_key->declarations.end(),
                    [&declaration](const ThemeToken& candidate) {
                        return candidate.name == declaration.name;
                    });
                if (existing_declaration == existing_key->declarations.end()) {
                    existing_key->declarations.push_back(declaration);
                } else {
                    *existing_declaration = declaration;
                }
            }
        }
    }
}

void append_rule(std::string& css, std::string_view selector,
                 const std::vector<ThemeToken>& declarations) {
    css += selector;
    css += " {\n";
    for (const ThemeToken& declaration : declarations) {
        css += "  ";
        css += declaration.name;
        css += ": ";
        css += declaration.value;
        css += ";\n";
    }
    css += "}\n";
}

} // namespace

std::optional<std::string> theme_key_selector(std::string_view class_name, std::string_view key) {
    if (class_name.empty()) {
        return std::nullopt;
    }
    // A key compiles to the classes and pseudo-classes the component already carries: the Button
    // sets `tbtn tbtn-primary`, so `variant:primary` is `.tbtn-primary` and not
    // `.tbtn.tbtn-primary` (the latter would also match a `tbtn` that is not primary, which is a
    // different rule).
    std::string selector;
    if (key == "base") {
        selector = ".";
        selector += class_name;
        return selector;
    }
    std::size_t start = 0;
    while (start <= key.size()) {
        const std::size_t plus = key.find('+', start);
        const std::string_view part = key.substr(
            start, plus == std::string_view::npos ? std::string_view::npos : plus - start);
        const std::size_t colon = part.find(':');
        if (colon == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view kind = part.substr(0, colon);
        const std::string_view value = part.substr(colon + 1);
        if (value.empty()) {
            return std::nullopt;
        }
        if (kind == "state") {
            // A state with no variant or size before it applies to the component itself.
            if (selector.empty()) {
                selector = ".";
                selector += class_name;
            }
            selector += ":";
            selector += value;
        } else if (kind == "variant" || kind == "size") {
            selector += ".";
            selector += class_name;
            selector += "-";
            selector += value;
        } else {
            return std::nullopt;
        }
        if (plus == std::string_view::npos) {
            break;
        }
        start = plus + 1;
    }
    return selector;
}

std::expected<ThemeBuild, ThemeSourceError>
build_theme_source(std::span<const ThemeSource* const> chain, const ThemeBuildOptions& options) {
    if (chain.empty()) {
        return std::unexpected(ThemeSourceError{ThemeSourceErrorCode::Extends,
                                                "a theme build needs at least one source"});
    }
    // The chain must link up: the first source extends nothing, and each later one extends the one
    // before it. A chain that does not is a build error rather than a silent partial merge, because
    // a theme that quietly lost its base is a look nobody authored.
    if (!chain.front()->extends.empty()) {
        return std::unexpected(ThemeSourceError{ThemeSourceErrorCode::Extends,
                                                "the first source `" + chain.front()->name +
                                                    "` extends `" + chain.front()->extends +
                                                    "`, which is not in the chain"});
    }
    for (std::size_t index = 1; index < chain.size(); ++index) {
        if (chain[index]->extends != chain[index - 1]->name) {
            return std::unexpected(ThemeSourceError{
                ThemeSourceErrorCode::Extends,
                "source `" + chain[index]->name + "` extends `" + chain[index]->extends +
                    "`, but the chain has `" + chain[index - 1]->name + "` before it"});
        }
    }

    Resolved resolved;
    for (const ThemeSource* source : chain) {
        merge_into(resolved, *source);
    }

    ThemeBuild build;
    std::vector<ThemeToken> token_declarations;
    token_declarations.reserve(resolved.tokens.size());
    for (const ThemeSourceToken& token : resolved.tokens) {
        if (is_colour_value(token.light) && is_colour_value(token.dark)) {
            token_declarations.push_back(
                ThemeToken{token.name, "light-dark(" + token.light + ", " + token.dark + ")"});
        } else {
            // A scheme-independent token is emitted once. Emitting `light-dark()` for a length
            // would be CSS the engine rejects, which is exactly the kind of gap the spike records.
            token_declarations.push_back(ThemeToken{token.name, token.light});
        }
    }

    build.css += "@layer ";
    build.css += options.layer;
    build.css += " {\n";
    std::string token_rule;
    append_rule(token_rule, options.selector, token_declarations);
    // Indent the whole block one level so the output reads as one layer.
    for (const char c : token_rule) {
        build.css.push_back(c);
        if (c == '\n') {
            build.css += "  ";
        }
    }
    for (const ThemeSourceComponent& component : resolved.components) {
        for (const ThemeSourceKey& key : component.keys) {
            const std::optional<std::string> selector =
                theme_key_selector(component.class_name, key.key);
            if (!selector) {
                return std::unexpected(ThemeSourceError{
                    ThemeSourceErrorCode::Key, "component `" + component.name + "` key `" +
                                                   key.key + "` does not compile to a selector"});
            }
            for (const ThemeToken& declaration : key.declarations) {
                collect_read_tokens(declaration.value, build.read_tokens);
            }
            std::string rule;
            append_rule(rule, *selector, key.declarations);
            for (const char c : rule) {
                build.css.push_back(c);
                if (c == '\n') {
                    build.css += "  ";
                }
            }
        }
    }
    build.css += "}\n";

    // The vocabulary cost: a token the theme defines that no component reads is a token the theme
    // pays for and nothing uses. It is a warning, because a theme may define a token for app CSS.
    for (const ThemeSourceToken& token : resolved.tokens) {
        if (std::find(build.read_tokens.begin(), build.read_tokens.end(), token.name) ==
            build.read_tokens.end()) {
            build.warnings.push_back("TESSERA_THEME_SOURCE_UNUSED_TOKEN: theme `" + resolved.name +
                                     "` defines `" + token.name +
                                     "`, which no component key reads");
        }
    }
    for (const std::string& name : build.read_tokens) {
        const auto found =
            std::find_if(resolved.tokens.begin(), resolved.tokens.end(),
                         [&name](const ThemeSourceToken& token) { return token.name == name; });
        if (found == resolved.tokens.end()) {
            build.warnings.push_back("TESSERA_THEME_SOURCE_UNKNOWN_TOKEN: a component reads `" +
                                     name + "`, which theme `" + resolved.name +
                                     "` does not define");
        }
    }
    return build;
}

} // namespace tessera::ui::style
