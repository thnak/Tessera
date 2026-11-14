// SPDX-License-Identifier: MIT

// Theme versions and pins (ADR-047 item 4, stylesheet-v1.md section 9.1, project-config-v1.md
// `assets.themePins`): the `themeVersions` parse, the SemVer pin rule, TESSERA_THEME_VERSION_PIN,
// and the version surviving the blob round trip. Deterministic: no files, clock or environment.

#include "tessera/ui/style/theme_asset.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace style = tessera::ui::style;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "theme_version: " << message << "\n";
        ++failures;
    }
}

constexpr std::string_view kVersioned = R"({
  "schemaVersion": 1,
  "themeVersions": { "light": "1.4.2", "dark": "2.0.0" },
  "themes": {
    "light": { "--bg": "#ffffff" },
    "dark": { "--bg": "#000000" }
  }
})";

void test_semver_shape() {
    check(style::is_theme_version("1.0.0"), "1.0.0 is a theme version");
    check(style::is_theme_version("10.20.30"), "multi-digit components are accepted");
    for (const std::string_view bad : {"", "1", "1.0", "1.0.0.0", "01.0.0", "1.0.x", "1.0.0-beta",
                                       "v1.0.0", "1..0", "1.0.", "-1.0.0", "1234567890.0.0"}) {
        check(!style::is_theme_version(bad), "`" + std::string(bad) + "` is not a theme version");
    }
    check(style::is_theme_version_pin("1"), "a major pin is valid");
    check(style::is_theme_version_pin("1.2"), "a major.minor pin is valid");
    check(style::is_theme_version_pin("1.2.3"), "a full pin is valid");
    check(!style::is_theme_version_pin("") && !style::is_theme_version_pin("^1") &&
              !style::is_theme_version_pin("1.2.3.4") && !style::is_theme_version_pin("01"),
          "malformed pins are rejected");
}

void test_pin_rule() {
    check(style::theme_version_satisfies_pin("1.4.2", "1"), "pin 1 accepts 1.4.2");
    check(style::theme_version_satisfies_pin("1.4.2", "1.4"), "pin 1.4 accepts 1.4.2");
    check(style::theme_version_satisfies_pin("1.4.2", "1.4.2"), "the full pin accepts itself");
    check(!style::theme_version_satisfies_pin("1.4.2", "1.5"), "pin 1.5 rejects 1.4.2");
    check(!style::theme_version_satisfies_pin("1.4.2", "1.4.3"), "pin 1.4.3 rejects 1.4.2");
    check(!style::theme_version_satisfies_pin("2.0.0", "1"), "pin 1 rejects 2.0.0");
    check(!style::theme_version_satisfies_pin("10.0.0", "1"),
          "pin 1 is a component prefix, so it rejects 10.0.0");
    check(!style::theme_version_satisfies_pin("11.0.0", "1.1"), "1.1 does not match 11.0.0");
    check(!style::theme_version_satisfies_pin("", "1"), "an unversioned theme satisfies no pin");
}

void test_parse_versions() {
    const auto parsed = style::parse_theme_asset(kVersioned);
    check(parsed.has_value(), "an asset with themeVersions parses");
    if (!parsed) {
        return;
    }
    check(parsed->find("light")->version == "1.4.2", "light carries its version");
    check(parsed->find("dark")->version == "2.0.0", "dark carries its version");
    check(parsed->find("default")->version.empty(), "an unlisted theme is unversioned");
    check(parsed->find("light")->find("--bg") != nullptr, "versions are not tokens");

    const auto plain =
        style::parse_theme_asset(R"({"schemaVersion":1,"themes":{"a":{"--x":"1"}}})");
    check(plain.has_value() && plain->find("a")->version.empty(),
          "an asset without themeVersions parses and is unversioned");

    const auto not_object =
        style::parse_theme_asset(R"({"schemaVersion":1,"themeVersions":[],"themes":{}})");
    check(!not_object && not_object.error().code == style::ThemeErrorCode::Version,
          "themeVersions must be an object");
    const auto bad_text = style::parse_theme_asset(
        R"({"schemaVersion":1,"themeVersions":{"a":"1.0"},"themes":{"a":{}}})");
    check(!bad_text && bad_text.error().code == style::ThemeErrorCode::Version,
          "a malformed version is TESSERA_THEME_VERSION");
    const auto unknown = style::parse_theme_asset(
        R"({"schemaVersion":1,"themeVersions":{"ghost":"1.0.0"},"themes":{"a":{}}})");
    check(!unknown && unknown.error().code == style::ThemeErrorCode::Id,
          "a version for an unknown theme is TESSERA_THEME_ID");
}

void test_check_pins() {
    const auto parsed = style::parse_theme_asset(kVersioned);
    if (!parsed) {
        check(false, "the versioned asset parses");
        return;
    }
    const std::vector<style::ThemePin> good{{"light", "1"}, {"dark", "2.0"}};
    check(!style::check_theme_pins(*parsed, good).has_value(), "matching pins pass");
    check(!style::check_theme_pins(*parsed, {}).has_value(), "no pins always passes");

    const std::vector<style::ThemePin> major{{"light", "1"}, {"dark", "1"}};
    const auto mismatch = style::check_theme_pins(*parsed, major);
    check(mismatch && mismatch->code == style::ThemeErrorCode::VersionPin,
          "a major mismatch is TESSERA_THEME_VERSION_PIN");
    check(mismatch && mismatch->message.find("dark") != std::string::npos &&
              mismatch->message.find("2.0.0") != std::string::npos &&
              mismatch->message.find("`1`") != std::string::npos,
          "the message names the theme and both versions");

    const std::vector<style::ThemePin> unknown{{"ghost", "1"}};
    check(style::check_theme_pins(*parsed, unknown).has_value(), "an unknown theme is a violation");
    const std::vector<style::ThemePin> unversioned{{"default", "1"}};
    const auto none = style::check_theme_pins(*parsed, unversioned);
    check(none && none->message.find("unversioned") != std::string::npos,
          "an unversioned theme cannot satisfy a pin");
    const std::vector<style::ThemePin> malformed{{"light", "^1"}};
    check(style::check_theme_pins(*parsed, malformed).has_value(), "a malformed pin is rejected");
    check(std::string(style::theme_error_code_name(style::ThemeErrorCode::VersionPin)) ==
              "TESSERA_THEME_VERSION_PIN",
          "the pin error code name");
}

void test_blob_keeps_versions() {
    const auto parsed = style::parse_theme_asset(kVersioned);
    if (!parsed) {
        check(false, "the versioned asset parses");
        return;
    }
    const std::vector<std::uint8_t> bytes = style::serialize_theme_asset(*parsed);
    const auto loaded = style::load_theme_asset_blob(bytes);
    check(loaded.has_value() && *loaded == *parsed, "the blob round-trips the theme versions");
    check(loaded && loaded->find("light")->version == "1.4.2",
          "the loaded theme keeps its version");
    check(style::serialize_theme_asset(*loaded) == bytes, "serialize(load(blob)) == blob");

    const std::string_view sheet_theme = "light";
    const style::PreparedStyleSheet prepared =
        style::prepare_theme_stylesheet(*parsed, sheet_theme);
    bool leaked = false;
    for (const style::PreparedItem& item : prepared.items) {
        for (const auto& declaration : item.declarations) {
            leaked = leaked || declaration.property.find("version") != std::string::npos;
        }
    }
    check(!leaked, "the version is never injected as a declaration");
}

} // namespace

int main() {
    test_semver_shape();
    test_pin_rule();
    test_parse_versions();
    test_check_pins();
    test_blob_keeps_versions();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "theme_version: ok\n";
    return 0;
}
