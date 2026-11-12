// SPDX-License-Identifier: MIT

// Theme asset model (docs/specs/stylesheet-v1.md section 9.1): the build-time parse of the JSON
// asset, the spec's typed errors and the TESSERA_THEME_TOKEN_MISSING warning list, and the
// versioned deterministic blob round-trip. Deterministic: no files, clock or environment.

#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace style = tessera::ui::style;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "theme_asset: " << message << "\n";
        ++failures;
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "theme_asset: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

// The spec's sample asset (stylesheet-v1.md section 9.1).
constexpr std::string_view kSample = R"({
  "schemaVersion": 1,
  "start": "light",
  "themes": {
    "light": { "--color-bg": "#ffffff", "--color-fg": "#111111" },
    "dark":  { "--color-bg": "#101014", "--color-fg": "#f2f2f2" }
  }
})";

std::string token_value(const style::ThemeAsset& asset, std::string_view id,
                        std::string_view name) {
    const style::Theme* theme = asset.find(id);
    if (theme == nullptr) {
        return "<no theme>";
    }
    const style::ThemeToken* token = theme->find(name);
    return token != nullptr ? token->value : "<no token>";
}

void expect_error(std::string_view json, style::ThemeErrorCode code, std::string_view message) {
    const auto parsed = style::parse_theme_asset(json);
    check(!parsed.has_value(), message);
    if (parsed) {
        return;
    }
    check_eq(style::theme_error_code_name(parsed.error().code), style::theme_error_code_name(code),
             message);
}

void test_sample() {
    const auto parsed = style::parse_theme_asset(kSample);
    check(parsed.has_value(), "the spec sample parses");
    if (!parsed) {
        return;
    }
    const style::ThemeAsset& asset = *parsed;
    check_eq(std::to_string(asset.schema_version), "1", "the sample is schemaVersion 1");
    check_eq(asset.start, "light", "the sample start is light");
    check_eq(std::to_string(asset.themes.size()), "3", "default plus the two authored themes");
    if (asset.themes.size() == 3) {
        check_eq(asset.themes[0].id, "default", "the built-in default is first");
        check_eq(asset.themes[1].id, "light", "document order, light");
        check_eq(asset.themes[2].id, "dark", "document order, dark");
    }
    check_eq(token_value(asset, "light", "--color-bg"), "#ffffff", "light background token");
    check_eq(token_value(asset, "light", "--color-fg"), "#111111", "light foreground token");
    check_eq(token_value(asset, "dark", "--color-bg"), "#101014", "dark background token");
    check_eq(token_value(asset, "dark", "--color-fg"), "#f2f2f2", "dark foreground token");
    // The built-in default defines exactly the reserved chrome pair.
    check_eq(token_value(asset, "default", "--window-background"), "#ffffff",
             "default window background");
    check_eq(token_value(asset, "default", "--window-foreground"), "#111111",
             "default window foreground");
    // Token order inside a theme is preserved.
    const style::Theme* light = asset.find("light");
    check(light != nullptr && light->tokens.size() == 2, "light has two tokens");
    if (light != nullptr && light->tokens.size() == 2) {
        check_eq(light->tokens[0].name, "--color-bg", "first token in document order");
        check_eq(light->tokens[1].name, "--color-fg", "second token in document order");
    }
    check(asset.warnings.empty(), "the sample has no token-missing warning");
}

void test_typed_errors() {
    expect_error(R"({"schemaVersion": 2, "themes": {}})", style::ThemeErrorCode::Version,
                 "a wrong schemaVersion is TESSERA_THEME_VERSION");
    expect_error(R"({"themes": {}})", style::ThemeErrorCode::Version,
                 "a missing schemaVersion is TESSERA_THEME_VERSION");
    expect_error(R"({"schemaVersion": 1})", style::ThemeErrorCode::Version,
                 "a missing themes object is TESSERA_THEME_VERSION");
    expect_error(R"({"schemaVersion": 1, "themes": []})", style::ThemeErrorCode::Version,
                 "a non-object themes is TESSERA_THEME_VERSION");
    expect_error(R"({"schemaVersion": 1, "themes": {"Light": {}}})", style::ThemeErrorCode::Id,
                 "an uppercase theme id is TESSERA_THEME_ID");
    expect_error(R"({"schemaVersion": 1, "themes": {"1light": {}}})", style::ThemeErrorCode::Id,
                 "a leading-digit theme id is TESSERA_THEME_ID");
    expect_error(R"({"schemaVersion": 1, "themes": {"light": []}})", style::ThemeErrorCode::Version,
                 "a theme that is not an object is TESSERA_THEME_VERSION");
    expect_error(R"({"schemaVersion": 1, "themes": {"light": {"color-bg": "#fff"}}})",
                 style::ThemeErrorCode::Version,
                 "a token without the -- prefix is TESSERA_THEME_VERSION");
    expect_error(R"({"schemaVersion": 1, "themes": {"light": {"--color-bg": 1}}})",
                 style::ThemeErrorCode::Version,
                 "a non-string token value is TESSERA_THEME_VERSION");
    expect_error(R"({"schemaVersion": 1, "themes": {"light": {"--color-bg": {"a": 1}}}})",
                 style::ThemeErrorCode::Version, "a nested token value is TESSERA_THEME_VERSION");
    expect_error(R"({"schemaVersion": 1, "start": "midnight", "themes": {"light": {}}})",
                 style::ThemeErrorCode::StartUnknown,
                 "an unknown start is TESSERA_THEME_START_UNKNOWN");
    expect_error(R"({"schemaVersion": 1, "start": 3, "themes": {}})",
                 style::ThemeErrorCode::Version, "a non-string start is TESSERA_THEME_VERSION");
    expect_error(R"({"schemaVersion": 1,)", style::ThemeErrorCode::Version,
                 "invalid JSON is TESSERA_THEME_VERSION");

    // `default` is always a known start, even when `themes.default` is absent.
    const auto default_start = style::parse_theme_asset(
        R"({"schemaVersion": 1, "start": "default", "themes": {"light": {}}})");
    check(default_start.has_value() && default_start->start == "default",
          "start default is accepted");
}

void test_token_missing_warning() {
    const auto parsed = style::parse_theme_asset(
        R"({"schemaVersion": 1, "themes": {
             "light": { "--color-bg": "#ffffff" },
             "dark": { "--color-fg": "#000000" }}})");
    check(parsed.has_value(), "a theme set with disjoint tokens parses");
    if (!parsed) {
        return;
    }
    check_eq(std::to_string(parsed->warnings.size()), "2", "one warning per missing token");
    bool dark_missing = false;
    bool light_missing = false;
    for (const std::string& warning : parsed->warnings) {
        check(warning.starts_with("TESSERA_THEME_TOKEN_MISSING:"),
              "the warning carries the spec code");
        dark_missing = dark_missing || (warning.find("theme `dark`") != std::string::npos &&
                                        warning.find("`--color-bg`") != std::string::npos);
        light_missing = light_missing || (warning.find("theme `light`") != std::string::npos &&
                                          warning.find("`--color-fg`") != std::string::npos);
    }
    check(dark_missing, "the warning names dark and --color-bg");
    check(light_missing, "the warning names light and --color-fg");

    // A reserved chrome token defined without its pair warns (stylesheet-v1.md section 9.2).
    const auto pair = style::parse_theme_asset(
        R"({"schemaVersion": 1, "themes": {"light": {"--window-background": "#fff"}}})");
    check(pair.has_value(), "a half chrome pair parses");
    if (pair) {
        check_eq(std::to_string(pair->warnings.size()), "1", "the half pair warns once");
        if (!pair->warnings.empty()) {
            check_eq(pair->warnings.front(),
                     "TESSERA_THEME_TOKEN_MISSING: theme `light` does not define "
                     "`--window-foreground`",
                     "the half-pair warning names the missing token");
        }
    }

    // An authored `themes.default` takes part in the scan, so a sibling is flagged.
    const auto authored = style::parse_theme_asset(
        R"({"schemaVersion": 1, "themes": {"default": {"--a": "1"}, "dark": {"--b": "2"}}})");
    check(authored.has_value(), "an authored default parses");
    if (authored) {
        check_eq(std::to_string(authored->themes.size()), "2",
                 "default is not duplicated when authored");
        check(authored->default_authored, "the asset records an authored default");
        check_eq(std::to_string(authored->warnings.size()), "2",
                 "the authored default participates in the scan");
    }
}

std::vector<std::uint8_t> copy_of(std::span<const std::uint8_t> blob) {
    return std::vector<std::uint8_t>(blob.begin(), blob.end());
}

// Rewrites the payload hash of a blob after the test mutated its payload.
void rehash(std::vector<std::uint8_t>& blob) {
    constexpr std::size_t kHeader = 20;
    constexpr std::size_t kHashOffset = 8;
    const std::string_view payload(reinterpret_cast<const char*>(blob.data() + kHeader),
                                   blob.size() - kHeader);
    const std::uint64_t hash = style::fnv1a64(payload);
    for (int i = 0; i < 8; ++i) {
        blob[kHashOffset + static_cast<std::size_t>(i)] =
            static_cast<std::uint8_t>(hash >> (8 * i));
    }
}

void put_u32(std::vector<std::uint8_t>& blob, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        blob[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

void test_blob_round_trip() {
    const auto parsed = style::parse_theme_asset(kSample);
    check(parsed.has_value(), "the sample parses for the blob test");
    if (!parsed) {
        return;
    }
    const std::vector<std::uint8_t> first = style::serialize_theme_asset(*parsed);
    const std::vector<std::uint8_t> second = style::serialize_theme_asset(*parsed);
    check(first == second, "repeated serialization is byte-identical");

    const auto loaded = style::load_theme_asset_blob(first);
    check(loaded.has_value(), "the serialized asset loads");
    if (!loaded) {
        return;
    }
    check(*loaded == *parsed, "the loaded asset equals the parsed asset");
    check(style::serialize_theme_asset(*loaded) == first,
          "re-serializing the loaded asset is byte-identical");
    check_eq(loaded->start, "light", "the loaded asset keeps start");
    check_eq(token_value(*loaded, "dark", "--color-bg"), "#101014",
             "the loaded asset keeps token values");

    // Truncation at every length is rejected without reading outside the bytes.
    for (std::size_t length = 0; length < first.size(); ++length) {
        const auto truncated =
            style::load_theme_asset_blob(std::span<const std::uint8_t>(first.data(), length));
        check(!truncated.has_value(), "a truncated blob is rejected");
        if (truncated) {
            continue;
        }
        check_eq(style::theme_blob_error_code_name(truncated.error().code), "Truncated",
                 "a truncated blob reports Truncated");
    }

    std::vector<std::uint8_t> bad_magic = copy_of(first);
    bad_magic[0] = 0;
    const auto magic = style::load_theme_asset_blob(bad_magic);
    check(!magic.has_value(), "a wrong magic is rejected");
    if (!magic) {
        check_eq(style::theme_blob_error_code_name(magic.error().code), "BadMagic",
                 "a wrong magic reports BadMagic");
    }

    std::vector<std::uint8_t> bad_version = copy_of(first);
    put_u32(bad_version, 4, 2);
    const auto version = style::load_theme_asset_blob(bad_version);
    check(!version.has_value(), "an unknown format version is rejected");
    if (!version) {
        check_eq(style::theme_blob_error_code_name(version.error().code),
                 "UnsupportedFormatVersion", "an unknown format version reports its code");
    }

    // A payload mutation without a re-hash is a hash mismatch.
    std::vector<std::uint8_t> corrupt_hash = copy_of(first);
    corrupt_hash[20] ^= 0xff;
    const auto hash = style::load_theme_asset_blob(corrupt_hash);
    check(!hash.has_value(), "a mutated payload is rejected");
    if (!hash) {
        check_eq(style::theme_blob_error_code_name(hash.error().code), "HashMismatch",
                 "a mutated payload reports HashMismatch");
    }

    // A mutated payload with a recomputed hash reaches the structural validation.
    std::vector<std::uint8_t> bad_schema = copy_of(first);
    put_u32(bad_schema, 20, 7);
    rehash(bad_schema);
    const auto schema = style::load_theme_asset_blob(bad_schema);
    check(!schema.has_value(), "an unsupported asset schema is rejected");
    if (!schema) {
        check_eq(style::theme_blob_error_code_name(schema.error().code), "Corrupt",
                 "an unsupported asset schema reports Corrupt");
    }

    // An absurd string length after a valid re-hash is caught by the bounds-checked reader.
    std::vector<std::uint8_t> bad_length = copy_of(first);
    // payload: u32 schema, u8 authored, string start (u32 length at payload offset 5).
    put_u32(bad_length, 20 + 5, 0xffffffffU);
    rehash(bad_length);
    const auto length = style::load_theme_asset_blob(bad_length);
    check(!length.has_value(), "an absurd string length is rejected");
    if (!length) {
        check_eq(style::theme_blob_error_code_name(length.error().code), "Corrupt",
                 "an absurd string length reports Corrupt");
    }

    // Trailing bytes after the payload are rejected too.
    std::vector<std::uint8_t> trailing = copy_of(first);
    trailing.push_back(0x00);
    put_u32(trailing, 16, static_cast<std::uint32_t>(trailing.size() - 20));
    rehash(trailing);
    const auto tail = style::load_theme_asset_blob(trailing);
    check(!tail.has_value(), "trailing bytes are rejected");
    if (!tail) {
        check_eq(style::theme_blob_error_code_name(tail.error().code), "Corrupt",
                 "trailing bytes report Corrupt");
    }
}

} // namespace

int main() {
    test_sample();
    test_typed_errors();
    test_token_missing_warning();
    test_blob_round_trip();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "theme_asset: ok\n";
    return 0;
}
