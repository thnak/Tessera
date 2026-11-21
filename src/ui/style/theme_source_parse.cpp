// SPDX-License-Identifier: MIT

// The theme source parser (spike #662, ADR-064 item 6). One JSON document in, one `ThemeSource`
// out; the build that turns it into CSS is `theme_source_build.cpp`.
//
// The format is deliberately small. ADR-064 item 6 asks for `name`, `extends`, `tokens`,
// `components` and `adaptations`; the spike implements the first four and records `adaptations` as
// a gap, because an adaptation compiles to a media query and the engine's media evaluation is
// already exercised by the gallery's density and text-scale switches.

#include "tessera/harness/json_parse.hpp"
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

namespace json = tessera::harness;

std::unexpected<ThemeSourceError> error(ThemeSourceErrorCode code, std::string message) {
    return std::unexpected(ThemeSourceError{code, std::move(message)});
}

// A theme or component name: the same `[a-z][a-z0-9-]*` shape a theme id takes, plus the
// PascalCase component names the catalogue uses. Both are checked by the same rule because both
// become part of a selector or an id.
bool is_name(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    const auto is_lower = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto is_upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    if (!is_lower(name.front()) && !is_upper(name.front())) {
        return false;
    }
    return std::all_of(name.begin() + 1, name.end(), [&](char c) {
        return is_lower(c) || is_upper(c) || (c >= '0' && c <= '9') || c == '-';
    });
}

// A component key is one of the closed forms of ADR-064 item 5: `base`, `variant:<v>`,
// `size:<s>`, `state:<s>`, and any `+`-joined combination of the last three. The build turns each
// into a selector, so an unknown form is an error rather than a silently dropped rule.
bool is_key(std::string_view key) {
    if (key == "base") {
        return true;
    }
    std::size_t start = 0;
    while (start <= key.size()) {
        const std::size_t plus = key.find('+', start);
        const std::string_view part = key.substr(
            start, plus == std::string_view::npos ? std::string_view::npos : plus - start);
        const std::size_t colon = part.find(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        const std::string_view kind = part.substr(0, colon);
        const std::string_view value = part.substr(colon + 1);
        if (kind != "variant" && kind != "size" && kind != "state") {
            return false;
        }
        if (!is_name(value)) {
            return false;
        }
        if (plus == std::string_view::npos) {
            break;
        }
        start = plus + 1;
    }
    return true;
}

std::optional<ThemeSourceError> read_declarations(const json::JsonValue& value,
                                                  std::string_view where,
                                                  std::vector<ThemeToken>& out) {
    const json::JsonValue::Object* members = value.as_object();
    if (members == nullptr) {
        return ThemeSourceError{ThemeSourceErrorCode::Shape,
                                "`" + std::string(where) + "` must be an object of declarations"};
    }
    out.reserve(members->size());
    for (const json::JsonValue::Member& member : *members) {
        const std::optional<std::string_view> text = member.second.as_string();
        if (!text) {
            return ThemeSourceError{ThemeSourceErrorCode::Shape, "declaration `" + member.first +
                                                                     "` in `" + std::string(where) +
                                                                     "` must be a string"};
        }
        out.push_back(ThemeToken{member.first, std::string(*text)});
    }
    return std::nullopt;
}

std::optional<ThemeSourceError> read_tokens(const json::JsonValue& value,
                                            std::vector<ThemeSourceToken>& out) {
    const json::JsonValue::Object* members = value.as_object();
    if (members == nullptr) {
        return ThemeSourceError{ThemeSourceErrorCode::Shape,
                                "`tokens` must be an object of token: value entries"};
    }
    out.reserve(members->size());
    for (const json::JsonValue::Member& member : *members) {
        if (member.first.size() < 3 || !member.first.starts_with("--")) {
            return ThemeSourceError{ThemeSourceErrorCode::Token,
                                    "token `" + member.first + "` must start with `--`"};
        }
        // A token is either a string (scheme-independent: a length, a font stack) or a two-element
        // array `[light, dark]` (a colour role, emitted as one `light-dark()` pair).
        if (const std::optional<std::string_view> single = member.second.as_string()) {
            out.push_back(
                ThemeSourceToken{member.first, std::string(*single), std::string(*single)});
            continue;
        }
        const json::JsonValue::Array* pair = member.second.as_array();
        if (pair == nullptr || pair->size() != 2) {
            return ThemeSourceError{ThemeSourceErrorCode::Shape,
                                    "token `" + member.first +
                                        "` must be a string or a [light, dark] pair"};
        }
        const std::optional<std::string_view> light = (*pair)[0].as_string();
        const std::optional<std::string_view> dark = (*pair)[1].as_string();
        if (!light || !dark) {
            return ThemeSourceError{ThemeSourceErrorCode::Shape,
                                    "token `" + member.first +
                                        "` must be a [light, dark] pair of strings"};
        }
        out.push_back(ThemeSourceToken{member.first, std::string(*light), std::string(*dark)});
    }
    return std::nullopt;
}

std::optional<ThemeSourceError> read_components(const json::JsonValue& value,
                                                std::vector<ThemeSourceComponent>& out) {
    const json::JsonValue::Object* members = value.as_object();
    if (members == nullptr) {
        return ThemeSourceError{ThemeSourceErrorCode::Shape,
                                "`components` must be an object of component entries"};
    }
    out.reserve(members->size());
    for (const json::JsonValue::Member& member : *members) {
        if (!is_name(member.first)) {
            return ThemeSourceError{ThemeSourceErrorCode::Name,
                                    "component `" + member.first + "` is not a valid name"};
        }
        const json::JsonValue::Object* fields = member.second.as_object();
        if (fields == nullptr) {
            return ThemeSourceError{ThemeSourceErrorCode::Shape,
                                    "component `" + member.first + "` must be an object"};
        }
        ThemeSourceComponent component;
        component.name = member.first;
        for (const json::JsonValue::Member& field : *fields) {
            if (field.first == "class") {
                const std::optional<std::string_view> text = field.second.as_string();
                if (!text || !is_name(*text)) {
                    return ThemeSourceError{ThemeSourceErrorCode::Name,
                                            "component `" + member.first +
                                                "` needs a valid `class` name"};
                }
                component.class_name = std::string(*text);
                continue;
            }
            if (field.first == "keys") {
                const json::JsonValue::Object* keys = field.second.as_object();
                if (keys == nullptr) {
                    return ThemeSourceError{ThemeSourceErrorCode::Shape,
                                            "component `" + member.first +
                                                "` `keys` must be an object"};
                }
                component.keys.reserve(keys->size());
                for (const json::JsonValue::Member& key : *keys) {
                    if (!is_key(key.first)) {
                        return ThemeSourceError{ThemeSourceErrorCode::Key,
                                                "component `" + member.first + "` key `" +
                                                    key.first + "` is not a known form"};
                    }
                    ThemeSourceKey entry;
                    entry.key = key.first;
                    if (const std::optional<ThemeSourceError> failed =
                            read_declarations(key.second, key.first, entry.declarations)) {
                        return failed;
                    }
                    component.keys.push_back(std::move(entry));
                }
                continue;
            }
            return ThemeSourceError{ThemeSourceErrorCode::Shape, "component `" + member.first +
                                                                     "` has an unknown member `" +
                                                                     field.first + "`"};
        }
        if (component.class_name.empty()) {
            return ThemeSourceError{ThemeSourceErrorCode::Shape,
                                    "component `" + member.first + "` needs a `class`"};
        }
        out.push_back(std::move(component));
    }
    return std::nullopt;
}

} // namespace

std::expected<ThemeSource, ThemeSourceError> parse_theme_source(std::string_view text) {
    const auto parsed = json::parse_json(text);
    if (!parsed) {
        return error(ThemeSourceErrorCode::Shape, "the theme source is not valid JSON");
    }
    const json::JsonValue::Object* members = parsed->as_object();
    if (members == nullptr) {
        return error(ThemeSourceErrorCode::Shape, "the theme source must be an object");
    }

    ThemeSource source;
    for (const json::JsonValue::Member& member : *members) {
        if (member.first == "name") {
            const std::optional<std::string_view> name = member.second.as_string();
            if (!name || !is_name(*name)) {
                return error(ThemeSourceErrorCode::Name, "`name` must be a valid theme name");
            }
            source.name = std::string(*name);
            continue;
        }
        if (member.first == "extends") {
            const std::optional<std::string_view> base = member.second.as_string();
            if (!base || !is_name(*base)) {
                return error(ThemeSourceErrorCode::Name, "`extends` must be a valid theme name");
            }
            source.extends = std::string(*base);
            continue;
        }
        if (member.first == "tokens") {
            if (const std::optional<ThemeSourceError> failed =
                    read_tokens(member.second, source.tokens)) {
                return std::unexpected(*failed);
            }
            continue;
        }
        if (member.first == "components") {
            if (const std::optional<ThemeSourceError> failed =
                    read_components(member.second, source.components)) {
                return std::unexpected(*failed);
            }
            continue;
        }
        // `adaptations` is the one member ADR-064 item 6 names that this spike does not build; it
        // is accepted and ignored so a source written against the full model still parses, and the
        // gap is recorded in the ADR rather than hidden behind a parse error.
        if (member.first == "adaptations") {
            continue;
        }
        return error(ThemeSourceErrorCode::Shape, "unknown member `" + member.first + "`");
    }
    if (source.name.empty()) {
        return error(ThemeSourceErrorCode::Name, "the theme source needs a `name`");
    }
    return source;
}

} // namespace tessera::ui::style
