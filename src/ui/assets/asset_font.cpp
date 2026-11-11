// SPDX-License-Identifier: MIT

#include "tessera/ui/assets/asset_font.hpp"

#include <cstddef>
#include <span>
#include <utility>

namespace tessera::ui::assets {
namespace {

[[nodiscard]] std::span<const std::uint8_t> as_u8(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

[[nodiscard]] AssetFontFailure failure(AssetFontError error, std::string_view asset) {
    return AssetFontFailure{error, std::string(asset)};
}

[[nodiscard]] AssetFontError lookup_error(AssetError error) noexcept {
    return error == AssetError::invalid_name ? AssetFontError::invalid_name
                                             : AssetFontError::not_found;
}

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    constexpr std::string_view kSpace = " \t\r";
    const std::size_t first = text.find_first_not_of(kSpace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kSpace) - first + 1);
}

} // namespace

std::expected<text::FontFace, AssetFontFailure>
load_asset_font(const AssetBundle& bundle, std::string_view name, std::uint32_t face_index) {
    const auto asset = bundle.find(name);
    if (!asset.has_value()) {
        return std::unexpected(failure(lookup_error(asset.error()), name));
    }
    auto face = text::FontFace::load_from_memory(as_u8(asset->bytes), face_index);
    if (!face.has_value()) {
        return std::unexpected(failure(AssetFontError::invalid_font, name));
    }
    return std::move(*face);
}

std::expected<std::vector<text::FontFace>, AssetFontFailure>
load_font_stack(const AssetBundle& bundle, std::string_view manifest_name) {
    const auto manifest = bundle.find(manifest_name);
    if (!manifest.has_value()) {
        return std::unexpected(failure(lookup_error(manifest.error()), manifest_name));
    }
    const std::string_view listing(reinterpret_cast<const char*>(manifest->bytes.data()),
                                   manifest->bytes.size());
    // A NUL means the asset is not the text file it claims to be.
    if (listing.find('\0') != std::string_view::npos) {
        return std::unexpected(failure(AssetFontError::malformed_manifest, manifest_name));
    }

    std::vector<text::FontFace> stack;
    std::size_t position = 0;
    while (position <= listing.size()) {
        const std::size_t end = listing.find('\n', position);
        const std::string_view line = trim(listing.substr(
            position, end == std::string_view::npos ? std::string_view::npos : end - position));
        position = end == std::string_view::npos ? listing.size() + 1 : end + 1;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (stack.size() == kMaxFontStackFaces) {
            return std::unexpected(failure(AssetFontError::malformed_manifest, manifest_name));
        }
        auto face = load_asset_font(bundle, line);
        if (!face.has_value()) {
            return std::unexpected(std::move(face.error()));
        }
        stack.push_back(std::move(*face));
    }
    if (stack.empty()) {
        return std::unexpected(failure(AssetFontError::empty_stack, manifest_name));
    }
    return stack;
}

} // namespace tessera::ui::assets
