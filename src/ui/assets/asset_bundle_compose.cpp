// SPDX-License-Identifier: MIT

#include "tessera/ui/assets/asset_bundle_compose.hpp"

namespace tessera::ui::assets {

std::expected<Asset, AssetError> ComposedAssetBundle::find(std::string_view name) const noexcept {
    // Validate once up front so the result does not depend on the list: a malformed name is always
    // invalid_name, including for an empty composition.
    const auto normalized = normalize_asset_name(name);
    if (!normalized.has_value()) {
        return std::unexpected(normalized.error());
    }
    for (const AssetBundle& bundle : bundles_) {
        auto asset = bundle.find(*normalized);
        if (asset.has_value()) {
            return asset; // the first bundle that contains the name wins
        }
        // Only not_found can remain: the name is already normalized, and AssetBundle::find treats
        // a corrupt table as missing. Continue to the next bundle in precedence order.
    }
    return std::unexpected(AssetError::not_found);
}

bool ComposedAssetBundle::contains(std::string_view name) const noexcept {
    return find(name).has_value();
}

} // namespace tessera::ui::assets
