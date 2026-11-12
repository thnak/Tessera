// SPDX-License-Identifier: MIT

// Build-time parse of the theme asset (docs/specs/stylesheet-v1.md section 9.1). The engine never
// parses this JSON at run time: the CLI parses it, serializes the asset, and the app loads the
// blob.

#include "tessera/harness/json_parse.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

namespace json = tessera::harness;

std::unexpected<ThemeError> error(ThemeErrorCode code, std::string message) {
    return std::unexpected(ThemeError{code, std::move(message)});
}

bool is_theme_id(std::string_view id) {
    if (id.empty() || id.front() < 'a' || id.front() > 'z') {
        return false;
    }
    return std::all_of(id.begin() + 1, id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

bool is_reserved_token(std::string_view name) {
    return name == kChromeBackgroundToken || name == kChromeForegroundToken;
}

// A token name is a custom-property name, so it must start with `--` (stylesheet-v1.md 9.1).
std::optional<ThemeError> read_tokens(const json::JsonValue& value, std::string_view theme_id,
                                      std::vector<ThemeToken>& tokens) {
    const json::JsonValue::Object* members = value.as_object();
    if (members == nullptr) {
        return ThemeError{ThemeErrorCode::Version,
                          "theme `" + std::string(theme_id) +
                              "` must be an object of token: value strings"};
    }
    tokens.reserve(members->size());
    for (const json::JsonValue::Member& member : *members) {
        if (member.first.size() < 3 || !member.first.starts_with("--")) {
            return ThemeError{ThemeErrorCode::Version, "token `" + member.first + "` in theme `" +
                                                           std::string(theme_id) +
                                                           "` must start with `--`"};
        }
        const std::optional<std::string_view> token_value = member.second.as_string();
        if (!token_value) {
            return ThemeError{ThemeErrorCode::Version, "token `" + member.first + "` in theme `" +
                                                           std::string(theme_id) +
                                                           "` must be a string"};
        }
        tokens.push_back(ThemeToken{member.first, std::string(*token_value)});
    }
    return std::nullopt;
}

void token_missing_warning(ThemeAsset& asset, std::string_view theme_id, std::string_view token) {
    asset.warnings.push_back("TESSERA_THEME_TOKEN_MISSING: theme `" + std::string(theme_id) +
                             "` does not define `" + std::string(token) + "`");
}

// `TESSERA_THEME_TOKEN_MISSING` (stylesheet-v1.md 9.1/9.2): a non-reserved token defined in some
// authored themes but not all of them, and a reserved token of the pair defined without the other.
// The implicit built-in `default` is not authored, so it is not expected to carry app tokens.
void compute_warnings(ThemeAsset& asset) {
    const std::size_t first = asset.default_authored ? 0 : 1;
    std::vector<std::string_view> union_names;
    for (std::size_t i = first; i < asset.themes.size(); ++i) {
        for (const ThemeToken& token : asset.themes[i].tokens) {
            if (is_reserved_token(token.name)) {
                continue;
            }
            if (std::find(union_names.begin(), union_names.end(), token.name) ==
                union_names.end()) {
                union_names.push_back(token.name);
            }
        }
    }
    for (const std::string_view name : union_names) {
        for (std::size_t i = first; i < asset.themes.size(); ++i) {
            if (asset.themes[i].find(name) == nullptr) {
                token_missing_warning(asset, asset.themes[i].id, name);
            }
        }
    }
    for (const Theme& theme : asset.themes) {
        const bool background = theme.find(kChromeBackgroundToken) != nullptr;
        const bool foreground = theme.find(kChromeForegroundToken) != nullptr;
        if (background && !foreground) {
            token_missing_warning(asset, theme.id, kChromeForegroundToken);
        } else if (foreground && !background) {
            token_missing_warning(asset, theme.id, kChromeBackgroundToken);
        }
    }
}

} // namespace

const ThemeToken* Theme::find(std::string_view name) const {
    for (const ThemeToken& token : tokens) {
        if (token.name == name) {
            return &token;
        }
    }
    return nullptr;
}

const Theme* ThemeAsset::find(std::string_view id) const {
    for (const Theme& theme : themes) {
        if (theme.id == id) {
            return &theme;
        }
    }
    return nullptr;
}

std::expected<ThemeAsset, ThemeError> parse_theme_asset(std::string_view text) {
    const auto parsed = json::parse_json(text);
    if (!parsed) {
        return error(ThemeErrorCode::Version,
                     "not valid JSON (offset " + std::to_string(parsed.error().offset) + ")");
    }
    const json::JsonValue& root = *parsed;
    if (!root.is_object()) {
        return error(ThemeErrorCode::Version, "the theme asset must be a JSON object");
    }
    const json::JsonValue* version = root.find("schemaVersion");
    if (version == nullptr) {
        return error(ThemeErrorCode::Version, "missing `schemaVersion` (expected 1)");
    }
    const std::optional<std::int64_t> version_value = version->as_integer();
    if (!version_value || *version_value != 1) {
        return error(ThemeErrorCode::Version, "unsupported `schemaVersion` (expected 1)");
    }
    const json::JsonValue* themes = root.find("themes");
    if (themes == nullptr || !themes->is_object()) {
        return error(ThemeErrorCode::Version, "missing `themes` object");
    }

    ThemeAsset asset;
    asset.schema_version = 1;
    asset.themes.push_back(default_theme_asset().themes.front());
    for (const json::JsonValue::Member& member : *themes->as_object()) {
        if (!is_theme_id(member.first)) {
            return error(ThemeErrorCode::Id,
                         "theme id `" + member.first + "` must match [a-z][a-z0-9-]*");
        }
        std::vector<ThemeToken> tokens;
        if (const std::optional<ThemeError> token_error =
                read_tokens(member.second, member.first, tokens)) {
            return std::unexpected(*token_error);
        }
        if (member.first == kDefaultThemeId) {
            asset.default_authored = true;
            asset.themes.front().tokens = std::move(tokens);
            continue;
        }
        asset.themes.push_back(Theme{member.first, std::move(tokens)});
    }

    const json::JsonValue* start = root.find("start");
    if (start != nullptr) {
        const std::optional<std::string_view> start_value = start->as_string();
        if (!start_value) {
            return error(ThemeErrorCode::Version, "`start` must be a theme id string");
        }
        if (*start_value != kDefaultThemeId && asset.find(*start_value) == nullptr) {
            return error(ThemeErrorCode::StartUnknown,
                         "start theme `" + std::string(*start_value) +
                             "` is neither `default` nor a key of `themes`");
        }
        asset.start = std::string(*start_value);
    }
    asset.warnings.clear();
    compute_warnings(asset);
    return asset;
}

} // namespace tessera::ui::style
