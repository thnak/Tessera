// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/text/font_face.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::assets {

// Failure modes of loading fonts from a bundle; every one is a value, never an exception.
enum class AssetFontError : std::uint8_t {
    not_found = 0,      // no such asset (the font itself or the stack manifest)
    invalid_name,       // an asset name is not normalized
    malformed_manifest, // the stack manifest is not text, or lists too many faces
    empty_stack,        // the manifest lists no face
    invalid_font,       // FreeType rejected the bytes
};

// The failing asset (the manifest itself when the whole request failed) so a bad bundle is
// diagnosable from the value alone.
struct AssetFontFailure {
    AssetFontError error = AssetFontError::not_found;
    std::string asset;
};

// The bundle asset that lists the default font stack.
inline constexpr std::string_view kDefaultFontStackManifest = "fonts/stack.txt";

// A stack names at most this many faces: a longer list is a packaging mistake, and every face is
// parsed at startup.
inline constexpr std::size_t kMaxFontStackFaces = 16;

// Loads one font by asset name. `face_index` selects a face inside a collection.
[[nodiscard]] std::expected<text::FontFace, AssetFontFailure>
load_asset_font(const AssetBundle& bundle, std::string_view name, std::uint32_t face_index = 0);

// Loads the font stack an app ships: the manifest asset lists one font asset name per line, in
// fallback order (the first face is the primary one; text no face covers falls through to the
// next). Blank lines and `#` comments are ignored and CRLF is tolerated. By convention the last
// entry is the emoji face, so emoji and ZWJ sequences render in color instead of as tofu
// (docs/design/text.md section 4). Pass the result to `FallbackShaper`.
[[nodiscard]] std::expected<std::vector<text::FontFace>, AssetFontFailure>
load_font_stack(const AssetBundle& bundle,
                std::string_view manifest_name = kDefaultFontStackManifest);

} // namespace tessera::ui::assets
