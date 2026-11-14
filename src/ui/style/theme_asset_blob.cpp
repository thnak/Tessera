// SPDX-License-Identifier: MIT

// Versioned deterministic blob of a parsed theme asset (docs/specs/compiled-stylesheet-v1.md
// "Theme asset blob"). The app loads this at startup instead of parsing JSON; the CLI writes it
// once at build time.

#include "blob_io.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style {
namespace {

using blob::Reader;
using blob::Writer;

// magic, format version, FNV-1a 64 of the payload, payload size.
inline constexpr char kThemeMagic[4] = {'T', 'T', 'H', 'B'};
inline constexpr std::size_t kThemeHeaderSize = 4 + 4 + 8 + 4;

void write_strings(Writer& out, std::span<const std::string> values) {
    out.count(values.size());
    for (const std::string& value : values) {
        out.raw_string(value);
    }
}

std::unexpected<ThemeBlobError> error(ThemeBlobErrorCode code, std::string message) {
    return std::unexpected(ThemeBlobError{code, std::move(message)});
}

void put_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void put_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

std::uint64_t payload_hash(std::span<const std::uint8_t> bytes) {
    return fnv1a64(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::vector<std::string> read_strings(Reader& in) {
    std::vector<std::string> values;
    const std::size_t count = in.count();
    values.reserve(count);
    for (std::size_t i = 0; i < count && !in.failed(); ++i) {
        values.push_back(in.raw_string());
    }
    return values;
}

} // namespace

std::vector<std::uint8_t> serialize_theme_asset(const ThemeAsset& asset) {
    Writer payload;
    payload.u32(asset.schema_version);
    payload.boolean(asset.default_authored);
    payload.raw_string(asset.start);
    write_strings(payload, asset.warnings);
    payload.count(asset.themes.size());
    for (const Theme& theme : asset.themes) {
        payload.raw_string(theme.id);
        payload.raw_string(theme.version);
        payload.count(theme.tokens.size());
        for (const ThemeToken& token : theme.tokens) {
            payload.raw_string(token.name);
            payload.raw_string(token.value);
        }
    }

    const std::vector<std::uint8_t>& body = payload.body();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kThemeHeaderSize + body.size());
    bytes.insert(bytes.end(), std::begin(kThemeMagic), std::end(kThemeMagic));
    put_u32(bytes, kThemeBlobFormatVersion);
    put_u64(bytes, payload_hash(body));
    put_u32(bytes, static_cast<std::uint32_t>(body.size()));
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::expected<ThemeAsset, ThemeBlobError>
load_theme_asset_blob(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kThemeHeaderSize) {
        return error(ThemeBlobErrorCode::Truncated, "theme blob is shorter than its header");
    }
    Reader header(bytes.first(kThemeHeaderSize));
    for (const char magic : kThemeMagic) {
        if (header.u8() != static_cast<std::uint8_t>(magic)) {
            return error(ThemeBlobErrorCode::BadMagic, "not a theme blob");
        }
    }
    const std::uint32_t format = header.u32();
    if (format != kThemeBlobFormatVersion) {
        return error(ThemeBlobErrorCode::UnsupportedFormatVersion,
                     "theme blob format version " + std::to_string(format) +
                         " is not supported (expected " + std::to_string(kThemeBlobFormatVersion) +
                         ")");
    }
    const std::uint64_t hash = header.u64();
    const std::uint32_t payload_size = header.u32();
    if (payload_size != bytes.size() - kThemeHeaderSize) {
        return error(ThemeBlobErrorCode::Truncated, "theme blob payload size does not match");
    }
    const std::span<const std::uint8_t> body = bytes.subspan(kThemeHeaderSize);
    if (payload_hash(body) != hash) {
        return error(ThemeBlobErrorCode::HashMismatch, "theme blob content hash differs");
    }

    Reader in(body);
    ThemeAsset asset;
    asset.schema_version = in.u32();
    if (asset.schema_version != 1) {
        return error(ThemeBlobErrorCode::Corrupt, "theme blob schema version " +
                                                      std::to_string(asset.schema_version) +
                                                      " is not supported");
    }
    asset.default_authored = in.boolean();
    asset.start = in.raw_string();
    asset.warnings = read_strings(in);
    const std::size_t theme_count = in.count();
    asset.themes.reserve(theme_count);
    for (std::size_t i = 0; i < theme_count && !in.failed(); ++i) {
        Theme theme;
        theme.id = in.raw_string();
        theme.version = in.raw_string();
        if (!theme.version.empty() && !is_theme_version(theme.version)) {
            return error(ThemeBlobErrorCode::Corrupt,
                         "theme blob carries a malformed theme version");
        }
        const std::size_t token_count = in.count();
        theme.tokens.reserve(token_count);
        for (std::size_t t = 0; t < token_count && !in.failed(); ++t) {
            ThemeToken token;
            token.name = in.raw_string();
            token.value = in.raw_string();
            theme.tokens.push_back(std::move(token));
        }
        asset.themes.push_back(std::move(theme));
    }
    if (in.failed()) {
        return error(ThemeBlobErrorCode::Corrupt, "theme blob is truncated or malformed");
    }
    if (in.remaining() != 0) {
        return error(ThemeBlobErrorCode::Corrupt, "theme blob has trailing bytes");
    }
    return asset;
}

} // namespace tessera::ui::style
