// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/theme_asset.hpp"

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The theme source of ADR-064 item 6 and docs/specs/theme-source-v1.md: the format a theme is
// authored in, and the build that turns it into plain CSS custom properties inside `@layer`.
//
// Spike #662. Nothing here runs in a shipped app: a theme is built at authoring time and the app
// loads the resulting CSS (or the theme asset blob) exactly as it does today. The point of the
// spike is to measure the model, not to add a runtime.
//
// Two properties the model is protecting, both from ADR-064 item 6:
//   * one token name per role, whose value is a `light-dark()` pair, so there are never separate
//     light and dark names; and
//   * the output is plain CSS custom properties inside `@layer`, so it is inspectable, needs no
//     runtime, and the cascade already knows how to resolve it.
namespace tessera::ui::style {

// One role token. `light` and `dark` are the two branches of the emitted `light-dark()` pair; a
// scheme-independent token (a length, a font stack) sets `dark` equal to `light` and is emitted as
// the bare value, because `light-dark()` on a length is not a colour and the engine would reject
// it.
struct ThemeSourceToken {
    std::string name; // `--color-background-surface`
    std::string light;
    std::string dark;

    friend bool operator==(const ThemeSourceToken&, const ThemeSourceToken&) = default;
};

// One component key: `base`, `variant:primary`, `variant:primary+size:sm`, `state:hover`. The
// declarations are ordinary CSS declarations in source order.
struct ThemeSourceKey {
    std::string key;
    std::vector<ThemeToken> declarations;

    friend bool operator==(const ThemeSourceKey&, const ThemeSourceKey&) = default;
};

// One component and the class its rules target. `class_name` is the styling target of ADR-064
// item 5: a theme reaches a component through a stable class, never through a generated one.
struct ThemeSourceComponent {
    std::string name;       // `Button`
    std::string class_name; // `tbtn`
    std::vector<ThemeSourceKey> keys;

    friend bool operator==(const ThemeSourceComponent&, const ThemeSourceComponent&) = default;
};

// A parsed theme source. `extends` names the theme this one derives from; the build resolves the
// chain into one flat theme, so a shipped theme is never a graph.
struct ThemeSource {
    std::string name;
    std::string extends;
    std::vector<ThemeSourceToken> tokens;
    std::vector<ThemeSourceComponent> components;

    friend bool operator==(const ThemeSource&, const ThemeSource&) = default;
};

enum class ThemeSourceErrorCode : std::uint8_t {
    Shape,   // TESSERA_THEME_SOURCE_SHAPE: not an object, a wrong-typed member
    Name,    // TESSERA_THEME_SOURCE_NAME: a theme or component name outside the id pattern
    Token,   // TESSERA_THEME_SOURCE_TOKEN: a token name that is not a custom property
    Key,     // TESSERA_THEME_SOURCE_KEY: a component key that is not one of the closed forms
    Extends, // TESSERA_THEME_SOURCE_EXTENDS: the chain does not link up
};

[[nodiscard]] constexpr const char* theme_source_error_code_name(ThemeSourceErrorCode code) {
    switch (code) {
    case ThemeSourceErrorCode::Shape:
        return "TESSERA_THEME_SOURCE_SHAPE";
    case ThemeSourceErrorCode::Name:
        return "TESSERA_THEME_SOURCE_NAME";
    case ThemeSourceErrorCode::Token:
        return "TESSERA_THEME_SOURCE_TOKEN";
    case ThemeSourceErrorCode::Key:
        return "TESSERA_THEME_SOURCE_KEY";
    case ThemeSourceErrorCode::Extends:
        return "TESSERA_THEME_SOURCE_EXTENDS";
    }
    return "TESSERA_THEME_SOURCE_SHAPE";
}

struct ThemeSourceError {
    ThemeSourceErrorCode code = ThemeSourceErrorCode::Shape;
    std::string message;
};

// Parses one theme source. Never throws; a malformed member is a typed error naming the member.
[[nodiscard]] std::expected<ThemeSource, ThemeSourceError>
parse_theme_source(std::string_view json);

struct ThemeBuildOptions {
    // The `@layer` the whole build lands in. A theme is layered so unlayered app CSS outranks it
    // regardless of specificity (ADR-063, ADR-064 item 6).
    std::string layer = "tessera-ui.theme";
    // The selector the token block is written under.
    std::string selector = ":root";
};

struct ThemeBuild {
    // The emitted CSS: one `@layer` block holding the token rule and one rule per component key.
    std::string css;
    // `TESSERA_THEME_SOURCE_UNUSED_TOKEN` and `TESSERA_THEME_SOURCE_UNKNOWN_TOKEN` findings. They
    // are warnings, not errors: a theme may define a token a component does not read yet, and a
    // component may read a token the engine's own defaults supply.
    std::vector<std::string> warnings;
    // The token names the components actually read, in first-use order. This is the vocabulary
    // cost the spike measures: how many tokens a theme defines against how many a control reads.
    std::vector<std::string> read_tokens;
};

// Builds `chain` (base first, derived last) into one flat theme. Each entry's `extends` must name
// the entry before it, and the first must not extend anything; a chain that does not link up is
// `Extends`. Later entries win: a token of the same name replaces the earlier one, and a component
// declaration of the same property replaces the earlier one, so a derived theme overrides without
// restating.
[[nodiscard]] std::expected<ThemeBuild, ThemeSourceError>
build_theme_source(std::span<const ThemeSource* const> chain,
                   const ThemeBuildOptions& options = {});

// The selector one component key compiles to, for `class_name` `tbtn`:
//   `base`                     -> `.tbtn`
//   `variant:primary`          -> `.tbtn-primary`
//   `variant:primary+size:sm`  -> `.tbtn-primary.tbtn-sm`
//   `state:hover`              -> `.tbtn:hover`
//   `variant:primary+state:hover` -> `.tbtn-primary:hover`
// Returns nullopt for a key outside those forms.
[[nodiscard]] std::optional<std::string> theme_key_selector(std::string_view class_name,
                                                            std::string_view key);

} // namespace tessera::ui::style
