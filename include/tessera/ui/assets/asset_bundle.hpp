// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

namespace tessera::ui::assets {

enum class AssetError : std::uint8_t {
    not_found = 0,
    invalid_name,
};

// Normalized asset-name rule, shared by the runtime lookup and the generator (kept in one header so
// both cannot drift):
// - '/' is the only separator; a single optional leading '/' is stripped; '\' is invalid;
// - no empty segments ("//"), no "." or ".." segments, no trailing '/';
// - allowed characters: A-Z a-z 0-9 '.' '_' '-' '/';
// - lookup is case-sensitive.
[[nodiscard]] inline std::expected<std::string_view, AssetError>
normalize_asset_name(std::string_view name) noexcept {
    if (!name.empty() && name.front() == '/') {
        name.remove_prefix(1);
    }
    if (name.empty()) {
        return std::unexpected(AssetError::invalid_name);
    }
    std::size_t segment_start = 0;
    for (std::size_t index = 0; index <= name.size(); ++index) {
        if (index == name.size() || name[index] == '/') {
            const std::string_view segment = name.substr(segment_start, index - segment_start);
            if (segment.empty() || segment == "." || segment == "..") {
                return std::unexpected(AssetError::invalid_name);
            }
            segment_start = index + 1;
            continue;
        }
        const char character = name[index];
        const bool allowed = (character >= 'a' && character <= 'z') ||
                             (character >= 'A' && character <= 'Z') ||
                             (character >= '0' && character <= '9') || character == '.' ||
                             character == '_' || character == '-';
        if (!allowed) {
            return std::unexpected(AssetError::invalid_name);
        }
    }
    return name;
}

// One generated bundle entry. `offset`/`size` slice into the bundle data blob.
struct AssetEntry {
    std::string_view name; // normalized
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::string_view mime; // optional hint, may be empty
};

struct Asset {
    std::string_view name;
    std::string_view mime;
    std::span<const std::byte> bytes;
};

// Read-only, zero-allocation view over a generated bundle: a sorted entry table plus one contiguous
// data blob. The generator sorts by normalized name and rejects duplicates, so lookup is a binary
// search; names are compared case-sensitively.
class AssetBundle {
public:
    constexpr AssetBundle(std::span<const AssetEntry> entries,
                          std::span<const std::byte> data) noexcept
        : entries_(entries),
          data_(data) {}

    // Returns the asset, `AssetError::invalid_name` for a malformed request, or `not_found`. No
    // allocation, no RTTI; the returned span points into the bundle's static data.
    [[nodiscard]] std::expected<Asset, AssetError> find(std::string_view name) const noexcept;

    [[nodiscard]] bool contains(std::string_view name) const noexcept;

    [[nodiscard]] std::span<const AssetEntry> entries() const noexcept {
        return entries_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return entries_.empty();
    }
    [[nodiscard]] std::size_t byte_size() const noexcept {
        return data_.size();
    }

private:
    std::span<const AssetEntry> entries_;
    std::span<const std::byte> data_;
};

} // namespace tessera::ui::assets
