// SPDX-License-Identifier: MIT

#include "tessera/ui/assets/asset_bundle.hpp"

#include <algorithm>
#include <cstddef>

namespace tessera::ui::assets {

std::expected<Asset, AssetError> AssetBundle::find(std::string_view name) const noexcept {
    const auto normalized = normalize_asset_name(name);
    if (!normalized.has_value()) {
        return std::unexpected(normalized.error());
    }
    const std::string_view target = *normalized;

    // Generated tables are sorted by normalized name; a hand-built table with the same invariant
    // works too. lower_bound keeps the lookup allocation-free and cache-friendly.
    const auto* first = entries_.data();
    const auto* last = first + entries_.size();
    const auto* found =
        std::lower_bound(first, last, target, [](const AssetEntry& entry, std::string_view value) {
            return entry.name < value;
        });
    if (found == last || found->name != target) {
        return std::unexpected(AssetError::not_found);
    }
    const std::uint64_t offset = found->offset;
    const std::uint64_t size = found->size;
    if (offset > data_.size() || size > data_.size() - offset) {
        // A corrupt table would otherwise hand out an out-of-bounds span; treat it as missing.
        return std::unexpected(AssetError::not_found);
    }
    return Asset{found->name, found->mime,
                 data_.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size))};
}

bool AssetBundle::contains(std::string_view name) const noexcept {
    return find(name).has_value();
}

} // namespace tessera::ui::assets
