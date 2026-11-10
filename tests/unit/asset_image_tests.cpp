// SPDX-License-Identifier: MIT
//
// AssetBundle -> image integration: name lookup, decode-cache identity (one decode per content),
// missing/invalid names, corrupt bytes and decode limits are values.

#include "tessera/ui/assets/asset_image.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

namespace assets = tessera::ui::assets;
namespace image = tessera::rendering::image;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "asset_image: " << message << "\n";
    }
    return condition;
}

[[nodiscard]] std::vector<std::uint8_t> read_fixture(const char* name) {
    const std::string path = std::string(TESSERA_IMAGE_FIXTURE_DIR) + "/" + name;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "asset_image: cannot open fixture: " << path << "\n";
        return {};
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(file)),
                                     std::istreambuf_iterator<char>());
}

struct BundleData {
    std::vector<std::uint8_t> corrupt;
    std::vector<std::uint8_t> checker;
    std::vector<std::uint8_t> jpeg;
    std::vector<std::uint8_t> storage;
    std::array<assets::AssetEntry, 3> entries;
    std::optional<assets::AssetBundle> bundle;

    BundleData()
        : corrupt({'n', 'o', 't', 'a', 'n', 'i', 'm', 'a', 'g', 'e'}),
          checker(read_fixture("checker.png")),
          jpeg(read_fixture("stripe.jpg")) {
        storage = corrupt;
        storage.insert(storage.end(), checker.begin(), checker.end());
        storage.insert(storage.end(), jpeg.begin(), jpeg.end());
        entries = {assets::AssetEntry{"images/bad.png", 0,
                                      static_cast<std::uint32_t>(corrupt.size()), "image/png"},
                   assets::AssetEntry{"images/checker.png",
                                      static_cast<std::uint32_t>(corrupt.size()),
                                      static_cast<std::uint32_t>(checker.size()), "image/png"},
                   assets::AssetEntry{"images/stripe.jpg",
                                      static_cast<std::uint32_t>(corrupt.size() + checker.size()),
                                      static_cast<std::uint32_t>(jpeg.size()), "image/jpeg"}};
        bundle.emplace(
            std::span<const assets::AssetEntry>(entries.data(), entries.size()),
            std::as_bytes(std::span<const std::uint8_t>(storage.data(), storage.size())));
    }
};

bool loads_and_decodes_once() {
    BundleData data;
    image::ImageDecodeCache cache;
    const auto first = assets::load_asset_image(*data.bundle, "images/checker.png", cache);
    bool ok = expect(first.has_value(), "the bundle image decodes") &&
              expect(first->get() != nullptr && (*first)->width == 8 && (*first)->height == 8,
                     "decoded dimensions match the fixture") &&
              expect((*first)->content_hash != 0, "the decode carries its content hash");
    if (first.has_value()) {
        ok = expect(cache.stats().decodes == 1 && cache.stats().hits == 0,
                    "the first load is a decode miss") &&
             ok;
        const auto second = assets::load_asset_image(*data.bundle, "images/checker.png", cache);
        ok = expect(second.has_value() && second->get() == first->get(),
                    "the second load returns the same cached decode") &&
             expect(cache.stats().decodes == 1 && cache.stats().hits == 1,
                    "the second load is a hit with no extra decode") &&
             ok;
    }
    const auto jpeg = assets::load_asset_image(*data.bundle, "images/stripe.jpg", cache);
    ok = expect(jpeg.has_value() && (*jpeg)->format == image::ImageFormat::jpeg,
                "JPEG assets load through the same path") &&
         ok;
    return ok;
}

bool failure_values() {
    BundleData data;
    image::ImageDecodeCache cache;
    const auto missing = assets::load_asset_image(*data.bundle, "images/missing.png", cache);
    bool ok = expect(!missing.has_value() && missing.error() == assets::AssetImageError::not_found,
                     "a missing asset is not_found");
    ok = expect(cache.stats().decodes == 0, "a missing asset does no decode") && ok;
    const auto invalid = assets::load_asset_image(*data.bundle, "../escape.png", cache);
    ok = expect(!invalid.has_value() && invalid.error() == assets::AssetImageError::invalid_name,
                "an invalid name is invalid_name") &&
         ok;
    const auto corrupt = assets::load_asset_image(*data.bundle, "images/bad.png", cache);
    ok = expect(!corrupt.has_value() &&
                    corrupt.error() == assets::AssetImageError::unsupported_format,
                "corrupt bytes report the decode value") &&
         expect(cache.stats().decodes == 0, "failed decodes are not counted or cached") && ok;

    image::ImageLimits tiny;
    tiny.max_input_bytes = 4;
    const auto capped = assets::load_asset_image(*data.bundle, "images/checker.png", cache, tiny);
    ok = expect(!capped.has_value() && capped.error() == assets::AssetImageError::input_too_large,
                "the decode limit is propagated") &&
         ok;
    return ok;
}

bool cached_decode_respects_caller_limits() {
    BundleData data;
    image::ImageDecodeCache cache;
    bool ok =
        expect(assets::load_asset_image(*data.bundle, "images/checker.png", cache).has_value(),
               "the image decodes under the default limits");
    image::ImageLimits narrow;
    narrow.max_width = 4; // the checker fixture is 8x8
    const auto capped = assets::load_asset_image(*data.bundle, "images/checker.png", cache, narrow);
    ok = expect(!capped.has_value() && capped.error() == assets::AssetImageError::limit_exceeded,
                "a cache hit still enforces a stricter width limit") &&
         ok;
    image::ImageLimits small_input;
    small_input.max_input_bytes = 4;
    const auto too_big =
        assets::load_asset_image(*data.bundle, "images/checker.png", cache, small_input);
    ok = expect(!too_big.has_value() && too_big.error() == assets::AssetImageError::input_too_large,
                "a cache hit still enforces a stricter input-size limit") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = loads_and_decodes_once() && ok;
    ok = failure_values() && ok;
    ok = cached_decode_respects_caller_limits() && ok;
    if (!ok) {
        std::cerr << "asset_image: FAILED\n";
        return 1;
    }
    std::cout << "asset_image: ok\n";
    return 0;
}
