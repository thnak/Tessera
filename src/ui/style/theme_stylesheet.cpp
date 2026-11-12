// SPDX-License-Identifier: MIT

// The built-in theme asset and the generated `:root` rule of the active theme
// (docs/specs/stylesheet-v1.md sections 9.1 and 9.2). The sheet is resolved through the ordinary
// cascade with `Origin::Theme`; there is no separate token lookup at paint time.

#include "tessera/ui/style/theme_asset.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

void append_token(PreparedItem& item, std::string_view name, std::string_view value) {
    item.declarations.push_back(Declaration{std::string(name), std::string(value), false, 0});
}

} // namespace

ThemeAsset default_theme_asset() {
    ThemeAsset asset;
    Theme theme;
    theme.id = std::string(kDefaultThemeId);
    theme.tokens.push_back(
        ThemeToken{std::string(kChromeBackgroundToken), std::string(kChromeBackgroundValue)});
    theme.tokens.push_back(
        ThemeToken{std::string(kChromeForegroundToken), std::string(kChromeForegroundValue)});
    asset.themes.push_back(std::move(theme));
    asset.start = std::string(kDefaultThemeId);
    return asset;
}

PreparedStyleSheet prepare_theme_stylesheet(const ThemeAsset& asset, std::string_view id) {
    PreparedStyleSheet prepared;
    const Theme* theme = asset.find(id);
    if (theme == nullptr) {
        return prepared;
    }

    PreparedItem item;
    item.kind = PreparedItemKind::Rule;
    PseudoClass root;
    root.name = "root";
    CompoundSelector compound;
    compound.pseudos.push_back(std::move(root));
    ComplexSelector selector;
    selector.compounds.push_back(std::move(compound));
    item.selectors.selectors.push_back(std::move(selector));
    for (const ThemeToken& token : theme->tokens) {
        append_token(item, token.name, token.value);
    }
    // Only the active theme is injected; a reserved token the theme leaves out still resolves to
    // the built-in value, because the pair is read outside the render tree (section 9.2).
    if (theme->find(kChromeBackgroundToken) == nullptr) {
        append_token(item, kChromeBackgroundToken, kChromeBackgroundValue);
    }
    if (theme->find(kChromeForegroundToken) == nullptr) {
        append_token(item, kChromeForegroundToken, kChromeForegroundValue);
    }
    prepared.items.push_back(std::move(item));
    return prepared;
}

} // namespace tessera::ui::style
