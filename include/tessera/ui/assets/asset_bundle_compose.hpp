// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/assets/asset_bundle.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <string_view>

namespace tessera::ui::assets {

// Ordered, allocation-free composition over several read-only bundles. This is the consumer-side
// adapter that lets an app whose project manifest bundle (fonts, images, theme) and generated
// vector bundle are separate `AssetBundle` values resolve either from one lookup, without asking
// the producers to change their output shape.
//
// Precedence rule: the FIRST bundle in the list that contains the normalized name wins; a name
// that also exists in a later bundle is shadowed by the earlier one, never an error. The order is
// the app's, so "generated vectors first, manifest second" makes a generated asset shadow a
// same-named manifest asset. Lookup is case-sensitive and follows normalize_asset_name().
//
// Failure is a value: a name in no bundle is AssetError::not_found, a malformed name is
// AssetError::invalid_name (reported before any bundle is consulted, so it does not depend on the
// list being non-empty), and an empty list is a valid composition that misses every name.
//
// The view borrows the caller's array of AssetBundle values: no allocation, no copying, no RTTI,
// and no per-name state. The returned span points into the winning bundle's read-only data, so the
// array and every bundle's entry table/blob must outlive the composition.
class ComposedAssetBundle {
public:
    constexpr explicit ComposedAssetBundle(std::span<const AssetBundle> bundles) noexcept
        : bundles_(bundles) {}

    // Returns the winning bundle's asset, `AssetError::invalid_name` for a malformed request, or
    // `AssetError::not_found` when no bundle contains the name.
    [[nodiscard]] std::expected<Asset, AssetError> find(std::string_view name) const noexcept;

    [[nodiscard]] bool contains(std::string_view name) const noexcept;

    // The borrowed bundle list, in precedence order.
    [[nodiscard]] std::span<const AssetBundle> bundles() const noexcept {
        return bundles_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return bundles_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return bundles_.empty();
    }

private:
    std::span<const AssetBundle> bundles_;
};

} // namespace tessera::ui::assets
