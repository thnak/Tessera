// SPDX-License-Identifier: MIT

// Deterministic JSON of a generated theme asset (issue #79). Hand-written rather than routed
// through the harness JSON codec because the codec's canonical form is a single line, and a token
// set is a file people read and edit: one token per line, two-space indent, asset order preserved.
// No floating point is ever emitted (every token value is a string), so the only way two runs can
// differ is a change of input.

#include "tessera/ui/style/theme_asset.hpp"
#include "tessera/ui/style/theme_generate.hpp"

#include <string>
#include <string_view>

namespace tessera::ui::style {
namespace {

// JSON string escaping for a token name or value: quotes, backslashes and the control characters
// JSON allows only in escaped form. Token values are CSS (`"IBM Plex Sans", sans-serif`), so the
// quote path is the one that matters.
void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char c : text) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                constexpr char kDigits[] = "0123456789abcdef";
                const auto value = static_cast<unsigned char>(c);
                out += "\\u00";
                out.push_back(kDigits[value >> 4]);
                out.push_back(kDigits[value & 0x0f]);
            } else {
                out.push_back(c);
            }
            break;
        }
    }
    out.push_back('"');
}

} // namespace

std::string format_theme_asset(const ThemeAsset& asset) {
    std::string out;
    out.reserve(4096);
    out += "{\n  \"schemaVersion\": ";
    out += std::to_string(asset.schema_version);
    out += ",\n  \"start\": ";
    append_json_string(out, asset.start);

    // `themeVersions` carries only the themes that have a version, the same rule the parser applies
    // when it reads the file back.
    bool any_version = false;
    for (const Theme& theme : asset.themes) {
        any_version = any_version || !theme.version.empty();
    }
    if (any_version) {
        out += ",\n  \"themeVersions\": {\n";
        bool first = true;
        for (const Theme& theme : asset.themes) {
            if (theme.version.empty()) {
                continue;
            }
            if (!first) {
                out += ",\n";
            }
            first = false;
            out += "    ";
            append_json_string(out, theme.id);
            out += ": ";
            append_json_string(out, theme.version);
        }
        out += "\n  }";
    }

    out += ",\n  \"themes\": {";
    bool first_theme = true;
    for (const Theme& theme : asset.themes) {
        // The built-in `default` theme is synthesized by the parser; writing it back would replace
        // the built-in chrome pair with whatever this asset happens to carry.
        if (theme.id == kDefaultThemeId && theme.tokens.size() <= 2) {
            continue;
        }
        out += first_theme ? "\n" : ",\n";
        first_theme = false;
        out += "    ";
        append_json_string(out, theme.id);
        out += ": {";
        bool first_token = true;
        for (const ThemeToken& token : theme.tokens) {
            out += first_token ? "\n" : ",\n";
            first_token = false;
            out += "      ";
            append_json_string(out, token.name);
            out += ": ";
            append_json_string(out, token.value);
        }
        out += "\n    }";
    }
    out += "\n  }\n}\n";
    return out;
}

} // namespace tessera::ui::style
