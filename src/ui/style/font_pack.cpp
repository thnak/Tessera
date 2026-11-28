// SPDX-License-Identifier: MIT

// The font pack manifest and the theme-side pin (docs/specs/theme-versions-v1.md section 5). Build time only.

#include "tessera/harness/json_parse.hpp"
#include "tessera/ui/style/font_pack.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace tessera::ui::style {
namespace {

namespace json = tessera::harness;

std::unexpected<FontPackError> fail(FontPackErrorCode code, std::string message) {
    return std::unexpected(FontPackError{code, std::move(message)});
}

bool is_pack_id(std::string_view id) {
    if (id.empty() || id.front() < 'a' || id.front() > 'z') {
        return false;
    }
    return std::all_of(id.begin() + 1, id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
    });
}

bool is_sha256_hex(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

std::optional<std::string> string_member(const json::JsonValue& object, std::string_view key) {
    const json::JsonValue* member = object.find(key);
    if (member == nullptr) {
        return std::nullopt;
    }
    const std::optional<std::string_view> text = member->as_string();
    if (!text) {
        return std::nullopt;
    }
    return std::string(*text);
}

std::expected<FontPackFile, FontPackError> read_font(const json::JsonValue& entry) {
    constexpr auto kBad = FontPackErrorCode::Manifest;
    if (!entry.is_object()) {
        return fail(kBad, "each `fonts` entry must be an object");
    }
    FontPackFile font;
    const auto file = string_member(entry, "file");
    const auto family = string_member(entry, "family");
    const auto style = string_member(entry, "style");
    const auto sha = string_member(entry, "sha256");
    const json::JsonValue* weight = entry.find("weight");
    const json::JsonValue* bytes = entry.find("bytes");
    if (!file || file->empty() || !family || family->empty()) {
        return fail(kBad, "a font needs a non-empty `file` and `family`");
    }
    if (file->find("..") != std::string::npos || file->front() == '/' ||
        file->find('\\') != std::string::npos) {
        return fail(kBad, "font file `" + *file + "` must be a relative path inside the pack");
    }
    if (!style || (*style != "normal" && *style != "italic")) {
        return fail(kBad, "font `" + *file + "` needs style `normal` or `italic`");
    }
    const std::optional<std::int64_t> weight_value =
        weight != nullptr ? weight->as_integer() : std::nullopt;
    if (!weight_value || *weight_value < 1 || *weight_value > 1000) {
        return fail(kBad, "font `" + *file + "` needs an integer `weight` in 1..1000");
    }
    const std::optional<std::int64_t> bytes_value =
        bytes != nullptr ? bytes->as_integer() : std::nullopt;
    if (!bytes_value || *bytes_value <= 0) {
        return fail(kBad, "font `" + *file + "` needs a positive `bytes`");
    }
    if (!sha || !is_sha256_hex(*sha)) {
        return fail(kBad, "font `" + *file + "` needs a 64-character lowercase hex `sha256`");
    }
    font.file = *file;
    font.family = *family;
    font.style = *style;
    font.sha256 = *sha;
    font.weight = static_cast<std::uint16_t>(*weight_value);
    font.bytes = static_cast<std::uint64_t>(*bytes_value);
    return font;
}

} // namespace

std::expected<FontPack, FontPackError> parse_font_pack(std::string_view text) {
    constexpr auto kBad = FontPackErrorCode::Manifest;
    const auto parsed = json::parse_json(text);
    if (!parsed) {
        return fail(kBad, "not valid JSON (offset " + std::to_string(parsed.error().offset) + ")");
    }
    const json::JsonValue& root = *parsed;
    const json::JsonValue* schema = root.is_object() ? root.find("schemaVersion") : nullptr;
    const std::optional<std::int64_t> schema_value =
        schema != nullptr ? schema->as_integer() : std::nullopt;
    if (!schema_value || *schema_value != 1) {
        return fail(kBad, "missing or unsupported `schemaVersion` (expected 1)");
    }
    FontPack pack;
    const auto id = string_member(root, "id");
    const auto version = string_member(root, "version");
    if (!id || !is_pack_id(*id)) {
        return fail(kBad, "`id` must match [a-z][a-z0-9-]*");
    }
    if (!version || !is_theme_version(*version)) {
        return fail(kBad, "`version` must be a MAJOR.MINOR.PATCH string");
    }
    pack.id = *id;
    pack.version = *version;
    const json::JsonValue* fonts = root.find("fonts");
    const json::JsonValue::Array* list = fonts != nullptr ? fonts->as_array() : nullptr;
    if (list == nullptr || list->empty()) {
        return fail(kBad, "`fonts` must be a non-empty array");
    }
    for (const json::JsonValue& entry : *list) {
        auto font = read_font(entry);
        if (!font) {
            return std::unexpected(font.error());
        }
        const bool repeated =
            std::any_of(pack.fonts.begin(), pack.fonts.end(),
                        [&](const FontPackFile& f) { return f.file == font->file; });
        if (repeated) {
            return fail(kBad, "font file `" + font->file + "` is listed twice");
        }
        pack.fonts.push_back(std::move(*font));
    }
    return pack;
}

std::expected<std::vector<ThemeFontPin>, FontPackError>
read_theme_font_pins(std::string_view theme_asset_json) {
    constexpr auto kBad = FontPackErrorCode::Pin;
    const auto parsed = json::parse_json(theme_asset_json);
    if (!parsed || !parsed->is_object()) {
        return fail(kBad, "the theme asset is not a JSON object");
    }
    std::vector<ThemeFontPin> pins;
    const json::JsonValue* packs = parsed->find("fontPacks");
    if (packs == nullptr) {
        return pins;
    }
    const json::JsonValue::Object* members = packs->as_object();
    if (members == nullptr) {
        return fail(kBad, "`fontPacks` must be an object of theme id to { id, version }");
    }
    for (const json::JsonValue::Member& member : *members) {
        const auto pack_id =
            member.second.is_object() ? string_member(member.second, "id") : std::nullopt;
        const auto version =
            member.second.is_object() ? string_member(member.second, "version") : std::nullopt;
        if (!pack_id || !version || !is_pack_id(*pack_id) || !is_theme_version(*version)) {
            return fail(kBad, "font pack pin of theme `" + member.first +
                                  "` must be { \"id\": pack id, \"version\": MAJOR.MINOR.PATCH }");
        }
        pins.push_back(ThemeFontPin{member.first, *pack_id, *version});
    }
    return pins;
}

std::optional<FontPackError> check_theme_font_pins(const ThemeAsset& asset,
                                                   std::span<const ThemeFontPin> pins,
                                                   const FontPack& pack) {
    constexpr auto kBad = FontPackErrorCode::Pin;
    for (const ThemeFontPin& pin : pins) {
        const Theme* theme = asset.find(pin.theme);
        if (theme == nullptr) {
            return FontPackError{kBad, "font pack pin names theme `" + pin.theme +
                                           "`, which the asset does not define"};
        }
        if (theme->version.empty()) {
            return FontPackError{kBad,
                                 "theme `" + pin.theme + "` pins a font pack but is unversioned"};
        }
        if (pin.pack_id != pack.id) {
            return FontPackError{kBad, "theme `" + pin.theme + "` pins font pack `" + pin.pack_id +
                                           "`, not `" + pack.id + "`"};
        }
        if (pin.pack_version != pack.version) {
            return FontPackError{kBad, "theme `" + pin.theme + "` (" + theme->version +
                                           ") pins font pack " + pack.id + " " + pin.pack_version +
                                           ", but the pack is " + pack.version};
        }
    }
    return std::nullopt;
}

} // namespace tessera::ui::style
