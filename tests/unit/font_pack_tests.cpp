// SPDX-License-Identifier: MIT

// The bundled font pack (ADR-047 item 4, issue #280, docs/specs/theme-versions-v1.md section 5): the manifest
// parses, every listed file is present with its pinned size and SHA-256, the OFL text travels with
// it, the Instrument themes pin the pack's exact version, and the text stack's tabular-number gap
// is recorded. Deterministic: only files of the repository, no clock, network or system fonts.

#include "tessera/ui/automation/sha256.hpp"
#include "tessera/ui/style/font_pack.hpp"
#include "tessera/ui/style/theme_asset.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace style = tessera::ui::style;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "font_pack: " << message << "\n";
        ++failures;
    }
}

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string pack_path(const std::string& relative) {
    return std::string(TESSERA_FONT_PACK_DIR) + "/" + relative;
}

std::optional<style::FontPack> load_pack() {
    const auto text = read_file(pack_path("font-pack.json"));
    if (!text) {
        check(false, "font-pack.json is readable");
        return std::nullopt;
    }
    auto pack = style::parse_font_pack(*text);
    if (!pack) {
        check(false, "font-pack.json parses: " + pack.error().message);
        return std::nullopt;
    }
    return *pack;
}

void test_shipped_pack(const style::FontPack& pack) {
    check(pack.id == "ibm-plex" && pack.version == "1.0.0", "the pack is ibm-plex 1.0.0");
    bool sans_regular = false;
    bool sans_bold = false;
    bool mono_regular = false;
    bool mono_heavier = false;
    for (const style::FontPackFile& font : pack.fonts) {
        sans_regular = sans_regular || (font.family == "IBM Plex Sans" && font.weight == 400);
        sans_bold = sans_bold || (font.family == "IBM Plex Sans" && font.weight >= 600);
        mono_regular = mono_regular || (font.family == "IBM Plex Mono" && font.weight == 400);
        mono_heavier = mono_heavier || (font.family == "IBM Plex Mono" && font.weight >= 600);

        const auto bytes = read_file(pack_path(font.file));
        if (!bytes) {
            check(false, font.file + " is present");
            continue;
        }
        check(bytes->size() == font.bytes, font.file + " has its pinned size");
        const auto digest =
            tessera::ui::automation::sha256(std::string_view(bytes->data(), bytes->size()));
        check(tessera::ui::automation::to_hex(digest) == font.sha256,
              font.file + " matches its pinned SHA-256");
    }
    check(sans_regular && sans_bold, "Plex Sans ships a regular and a heavier weight");
    check(mono_regular && mono_heavier, "Plex Mono ships a regular and a heavier weight");

    const auto license = read_file(pack_path("OFL.txt"));
    check(license && license->find("SIL OPEN FONT LICENSE Version 1.1") != std::string::npos,
          "OFL.txt travels with the pack");
    check(license && license->find("Reserved Font Name \"Plex\"") != std::string::npos,
          "the Reserved Font Name notice is kept");
}

void test_manifest_errors() {
    const std::string good =
        R"({"schemaVersion":1,"id":"p","version":"1.0.0","fonts":[{"file":"a.ttf","family":"A",)"
        R"("weight":400,"style":"normal","sha256":")" +
        std::string(64, 'a') + R"(","bytes":10}]})";
    check(style::parse_font_pack(good).has_value(), "a minimal manifest parses");
    const auto replaced = [&](std::string_view from, std::string_view to) {
        std::string text = good;
        text.replace(text.find(from), from.size(), to);
        return text;
    };
    const std::vector<std::string> bad{
        "not json",
        replaced("\"schemaVersion\":1", "\"schemaVersion\":2"),
        replaced("\"id\":\"p\"", "\"id\":\"P\""),
        replaced("1.0.0", "1.0"),
        replaced("\"weight\":400", "\"weight\":0"),
        replaced("\"style\":\"normal\"", "\"style\":\"oblique\""),
        replaced("a.ttf", "../a.ttf"),
        replaced(std::string(64, 'a'), std::string(63, 'a')),
        replaced(std::string(64, 'a'), std::string(64, 'A')),
        replaced("\"bytes\":10", "\"bytes\":0"),
        replaced("[{", "[1,{"),
        replaced("\"fonts\":[", "\"fonts\":[],\"x\":["),
    };
    for (const std::string& text : bad) {
        const auto parsed = style::parse_font_pack(text);
        check(!parsed.has_value() && parsed.error().code == style::FontPackErrorCode::Manifest,
              "a malformed manifest is rejected: " + text.substr(0, 40));
    }
    const std::string twice =
        replaced("}]}", "},{\"file\":\"a.ttf\",\"family\":\"A\",\"weight\":700,"
                        "\"style\":\"normal\",\"sha256\":\"" +
                            std::string(64, 'b') + "\",\"bytes\":1}]}");
    check(!style::parse_font_pack(twice).has_value(), "a repeated file is rejected");
}

void test_theme_pins(const style::FontPack& pack) {
    const auto theme_text = read_file(TESSERA_INSTRUMENT_THEME);
    if (!theme_text) {
        check(false, "the Instrument theme asset is readable");
        return;
    }
    const auto asset = style::parse_theme_asset(*theme_text);
    const auto pins = style::read_theme_font_pins(*theme_text);
    if (!asset || !pins) {
        check(false, "the Instrument theme asset and its pins parse");
        return;
    }
    check(pins->size() == 3, "all three Instrument themes pin the pack");
    check(!style::check_theme_font_pins(*asset, *pins, pack).has_value(),
          "the shipped pins match the shipped pack");
    for (const style::ThemeFontPin& pin : *pins) {
        check(pin.pack_id == "ibm-plex" && pin.pack_version == pack.version,
              pin.theme + " pins ibm-plex " + pack.version);
    }

    style::FontPack bumped = pack;
    bumped.version = "1.0.1";
    const auto stale = style::check_theme_font_pins(*asset, *pins, bumped);
    check(stale && stale->code == style::FontPackErrorCode::Pin &&
              stale->message.find("1.0.0") != std::string::npos &&
              stale->message.find("1.0.1") != std::string::npos,
          "a pack bump the theme did not take is TESSERA_FONT_PACK_PIN naming both versions");

    style::FontPack other = pack;
    other.id = "other";
    check(style::check_theme_font_pins(*asset, *pins, other).has_value(),
          "a pin for another pack id is a violation");
    const std::vector<style::ThemeFontPin> ghost{{"ghost", "ibm-plex", "1.0.0"}};
    check(style::check_theme_font_pins(*asset, ghost, pack).has_value(),
          "a pin naming an unknown theme is a violation");
    const std::vector<style::ThemeFontPin> unversioned{{"default", "ibm-plex", "1.0.0"}};
    const auto none = style::check_theme_font_pins(*asset, unversioned, pack);
    check(none && none->message.find("unversioned") != std::string::npos,
          "an unversioned theme cannot pin a pack");
    check(!style::check_theme_font_pins(*asset, {}, pack).has_value(), "no pins always passes");

    check(style::read_theme_font_pins(R"({"schemaVersion":1,"themes":{}})")->empty(),
          "a theme asset without fontPacks is unpinned");
    check(!style::read_theme_font_pins(R"({"fontPacks":{"a":{"id":"p"}}})").has_value(),
          "a pin without a version is malformed");
    check(!style::read_theme_font_pins(R"({"fontPacks":[]})").has_value(),
          "fontPacks must be an object");
    check(std::string(style::font_pack_error_code_name(style::FontPackErrorCode::Pin)) ==
              "TESSERA_FONT_PACK_PIN",
          "the pin error code name");
}

// ADR-047 item 5 asks for OpenType `tnum`. The shaper calls hb_shape with no feature list, so
// `tnum` and `font-feature-settings` are NOT applied (gap, recorded in theme-versions-v1.md section 5 and filed as
// an issue). For this pack it is not observable: every Plex face's default figures are already
// tabular, so all ten digits advance alike without the feature. This test pins that fact for each
// shipped file; a face whose default figures were proportional would fail it, which is the signal
// to implement feature support first.
void test_default_digits_are_tabular(const style::FontPack& pack) {
    tessera::ui::text::TextStyle style;
    style.font_size = 100.0f;
    for (const style::FontPackFile& font : pack.fonts) {
        const auto face = tessera::ui::text::FontFace::load_from_file(pack_path(font.file));
        if (!face) {
            check(false, font.file + " loads through the text stack");
            continue;
        }
        const tessera::ui::text::HarfBuzzShaper shaper(*face);
        const float zero = shaper.shape("0", style).advance;
        check(zero > 0.0f, font.file + " shapes digits");
        for (char digit = '1'; digit <= '9'; ++digit) {
            check(shaper.shape(std::string(1, digit), style).advance == zero,
                  font.file + ": digit " + digit + " advances like 0 with no tnum feature");
        }
    }
}

} // namespace

int main() {
    const std::optional<style::FontPack> pack = load_pack();
    if (pack) {
        test_shipped_pack(*pack);
        test_theme_pins(*pack);
        test_default_digits_are_tabular(*pack);
    }
    test_manifest_errors();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "font_pack: ok\n";
    return 0;
}
