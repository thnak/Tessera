// SPDX-License-Identifier: MIT
//
// ImageTextureCache contract: one upload per (content hash, raster extent, format, filter),
// deterministic nearest resample to the requested extent, stable handles, bounded LRU eviction,
// entry-size guard, upload-failure value and D7 generation invalidation.

#include "path_texture_cache_test_fakes.hpp"
#include "tessera/rendering/image/image_texture_cache.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

namespace image = tessera::rendering::image;
namespace rhi = tessera::rendering::rhi;
using tessera::test::path_texture::FakeDevice;
using tessera::test::path_texture::FakeTexture;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "image_texture_cache: " << message << "\n";
    }
    return condition;
}

[[nodiscard]] image::DecodedImage solid_image(std::uint32_t width, std::uint32_t height,
                                              std::uint8_t red, std::uint8_t green,
                                              std::uint8_t blue, std::uint8_t alpha) {
    image::DecodedImage decoded;
    decoded.width = width;
    decoded.height = height;
    decoded.content_hash = 0xC0FFEEULL;
    decoded.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
            decoded.rgba[index] = red;
            decoded.rgba[index + 1] = green;
            decoded.rgba[index + 2] = blue;
            decoded.rgba[index + 3] = alpha;
        }
    }
    return decoded;
}

[[nodiscard]] const FakeTexture* as_fake(const image::CachedImageTexture& cached) {
    return dynamic_cast<const FakeTexture*>(cached.texture);
}

bool hit_and_upload_once() {
    FakeDevice device;
    image::ImageTextureCache cache(device);
    const image::DecodedImage decoded = solid_image(4, 4, 10, 20, 30, 40);
    const image::ImageTextureKey key{decoded.content_hash, 4, 4, rhi::TextureFormat::R8G8B8A8Unorm};
    const auto first = cache.acquire(decoded, key);
    bool ok = expect(first.has_value(), "a valid request uploads") &&
              expect(cache.stats().uploads == 1, "one texture creation") &&
              expect(first->extent.width == 4 && first->extent.height == 4, "extent matches key");
    if (first.has_value()) {
        const FakeTexture* texture = as_fake(*first);
        ok = expect(texture != nullptr && texture->bytes() == decoded.rgba,
                    "1:1 upload stores the decoded RGBA bytes unchanged") &&
             ok;
    }
    const auto second = cache.acquire(decoded, key);
    ok = expect(second.has_value() && cache.stats().uploads == 1 && cache.stats().hits == 1,
                "the second request is a cache hit with no upload") &&
         expect(second.has_value() && first.has_value() && second->texture == first->texture,
                "the hit returns the same stable texture") &&
         ok;
    return ok;
}

// The sampling filter is part of the identity (S8): sampler state is fixed at texture creation, so
// one image drawn at an aligned 1:1 position and at a scaled position needs two entries. Both
// variants are uploads of the same bytes and the cache bounds count them separately.
bool filter_is_part_of_the_identity() {
    FakeDevice device;
    image::ImageTextureCache cache(device);
    const image::DecodedImage decoded = solid_image(4, 4, 10, 20, 30, 255);
    image::ImageTextureKey nearest_key{decoded.content_hash, 4, 4,
                                       rhi::TextureFormat::R8G8B8A8Unorm};
    image::ImageTextureKey linear_key = nearest_key;
    linear_key.filter = rhi::Filter::Linear;

    const auto nearest = cache.acquire(decoded, nearest_key);
    const auto linear = cache.acquire(decoded, linear_key);
    bool ok =
        expect(nearest.has_value() && linear.has_value(), "both filter variants upload") &&
        expect(cache.stats().uploads == 2 && cache.entry_count() == 2,
               "the filter is part of the key: one entry per variant") &&
        expect(nearest.has_value() && as_fake(*nearest) != nullptr &&
                   as_fake(*nearest)->filter() == rhi::Filter::Nearest,
               "the nearest entry's sampler state is nearest") &&
        expect(linear.has_value() && as_fake(*linear) != nullptr &&
                   as_fake(*linear)->filter() == rhi::Filter::Linear,
               "the linear entry's sampler state is linear") &&
        expect(nearest.has_value() && linear.has_value() && nearest->texture != linear->texture,
               "the two variants are distinct textures");

    // Re-acquiring either variant is a hit; a different filter never aliases the other entry.
    const auto nearest_again = cache.acquire(decoded, nearest_key);
    ok = expect(nearest_again.has_value() && cache.stats().hits == 1 &&
                    cache.stats().uploads == 2 && nearest_again->texture == nearest->texture,
                "a repeated variant hits its own entry") &&
         ok;
    return ok;
}

// The mip level count is part of the identity too (RHI 1.6, #491): the sampler's LOD range is fixed
// at creation. A chain entry counts every level against the byte limit, and a request the device
// cannot honour is never silently downgraded by the cache.
bool levels_are_part_of_the_identity() {
    FakeDevice device;
    image::ImageTextureCache cache(device);
    const image::DecodedImage decoded = solid_image(4, 4, 10, 20, 30, 255);
    image::ImageTextureKey flat_key{decoded.content_hash, 4, 4, rhi::TextureFormat::R8G8B8A8Unorm};
    image::ImageTextureKey chain_key = flat_key;
    chain_key.levels = 3; // 4x4 -> 2x2 -> 1x1

    const auto flat = cache.acquire(decoded, flat_key);
    const std::uint64_t flat_bytes = cache.byte_count();
    const auto chain = cache.acquire(decoded, chain_key);
    bool ok =
        expect(flat.has_value() && chain.has_value(), "both level variants upload") &&
        expect(cache.stats().uploads == 2 && cache.entry_count() == 2,
               "the level count is part of the key: one entry per variant") &&
        expect(flat.has_value() && as_fake(*flat) != nullptr && as_fake(*flat)->levels() == 1,
               "the flat entry has one level") &&
        expect(chain.has_value() && as_fake(*chain) != nullptr && as_fake(*chain)->levels() == 3,
               "the chain entry is created with the requested level count") &&
        expect(cache.byte_count() - flat_bytes == (16U + 4U + 1U) * 4U,
               "a chain entry counts every level against the byte limit");

    image::ImageTextureKey none = flat_key;
    none.levels = 0;
    image::ImageTextureKey too_many = flat_key;
    too_many.levels = 4; // a 4x4 raster has three levels
    const auto rejected_none = cache.acquire(decoded, none);
    const auto rejected_many = cache.acquire(decoded, too_many);
    ok = expect(!rejected_none.has_value() &&
                    rejected_none.error() == image::ImageTextureError::invalid_request,
                "zero levels is an invalid request") &&
         expect(!rejected_many.has_value() &&
                    rejected_many.error() == image::ImageTextureError::invalid_request,
                "more levels than a full chain is an invalid request") &&
         ok;

    ok = expect(cache.supports_mipmaps(), "the cache reports the device's capability") && ok;
    device.mipmaps_supported = false;
    ok = expect(!cache.supports_mipmaps(), "and follows a device that cannot build a chain") && ok;
    return ok;
}

bool resamples_to_requested_extent() {
    FakeDevice device;
    image::ImageTextureCache cache(device);
    // 2x2 source: red, green / blue, white. Blitting to 4x4 must give 2x2 blocks.
    image::DecodedImage decoded;
    decoded.width = 2;
    decoded.height = 2;
    decoded.content_hash = 0x1234ULL;
    decoded.rgba = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255,
    };
    const image::ImageTextureKey key{decoded.content_hash, 4, 4, rhi::TextureFormat::R8G8B8A8Unorm};
    const auto cached = cache.acquire(decoded, key);
    if (!expect(cached.has_value(), "the scaled upload succeeds")) {
        return false;
    }
    const FakeTexture* texture = as_fake(*cached);
    if (!expect(texture != nullptr, "the upload is a fake texture")) {
        return false;
    }
    const auto& bytes = texture->bytes();
    bool ok = expect(bytes.size() == 4U * 4U * 4U, "the upload is 4x4 RGBA8");
    const auto pixel = [&bytes](std::uint32_t x, std::uint32_t y, std::uint32_t channel) {
        return bytes[(static_cast<std::size_t>(y) * 4 + x) * 4 + channel];
    };
    ok = expect(pixel(0, 0, 0) == 255 && pixel(0, 0, 1) == 0, "top-left block is red") &&
         expect(pixel(3, 0, 1) == 255 && pixel(3, 0, 0) == 0, "top-right block is green") &&
         expect(pixel(0, 3, 2) == 255, "bottom-left block is blue") &&
         expect(pixel(3, 3, 0) == 255 && pixel(3, 3, 2) == 255, "bottom-right block is white") &&
         ok;
    return ok;
}

// The CPU resample is the measured cost (about 50 ms for a 4K image), so the counter that makes it
// observable is part of the contract: an extent equal to the decoded extent never resamples, a
// different extent resamples once per upload and a cache hit never resamples at all.
bool resample_is_skipped_when_the_extent_matches() {
    FakeDevice device;
    image::ImageTextureCache cache(device);
    image::DecodedImage decoded = solid_image(6, 4, 1, 2, 3, 255);
    // Distinct neighbors, so a wrong mapping at an equal extent would change the bytes.
    for (std::size_t i = 0; i < decoded.rgba.size(); ++i) {
        decoded.rgba[i] = static_cast<std::uint8_t>(i * 7U + 3U);
    }
    const image::ImageTextureKey same{decoded.content_hash, 6, 4,
                                      rhi::TextureFormat::R8G8B8A8Unorm};
    const auto identity = cache.acquire(decoded, same);
    bool ok = expect(identity.has_value() && cache.stats().resamples == 0,
                     "an equal extent uploads without a CPU resample");
    if (identity.has_value()) {
        const FakeTexture* texture = as_fake(*identity);
        ok = expect(texture != nullptr && texture->bytes() == decoded.rgba,
                    "the equal-extent upload is byte-identical to the decode") &&
             ok;
    }

    // A mip chain at the equal extent still skips the resample (the device builds the levels).
    image::ImageTextureKey chain = same;
    chain.levels = 3;
    const auto chained = cache.acquire(decoded, chain);
    ok = expect(chained.has_value() && cache.stats().resamples == 0,
                "a mip chain at the equal extent does not resample either") &&
         ok;

    image::ImageTextureKey smaller{decoded.content_hash, 3, 2, rhi::TextureFormat::R8G8B8A8Unorm};
    const auto scaled = cache.acquire(decoded, smaller);
    ok = expect(scaled.has_value() && cache.stats().resamples == 1,
                "a different extent resamples once") &&
         ok;
    static_cast<void>(cache.acquire(decoded, smaller));
    static_cast<void>(cache.acquire(decoded, same));
    ok = expect(cache.stats().resamples == 1, "cache hits never resample") && ok;

    // One axis different is still a resample: the shortcut needs both extents equal.
    image::ImageTextureKey taller{decoded.content_hash, 6, 8, rhi::TextureFormat::R8G8B8A8Unorm};
    const auto stretched = cache.acquire(decoded, taller);
    ok = expect(stretched.has_value() && cache.stats().resamples == 2,
                "a single differing axis still resamples") &&
         ok;
    return ok;
}

bool eviction_and_entry_guard() {
    FakeDevice device;
    image::ImageTextureCacheConfig config;
    config.max_entries = 1;
    image::ImageTextureCache cache(device, config);
    const image::DecodedImage first_image = solid_image(4, 4, 1, 2, 3, 255);
    image::DecodedImage second_image = solid_image(4, 4, 4, 5, 6, 255);
    second_image.content_hash = 0xBEEFULL;
    const auto first = cache.acquire(
        first_image, {first_image.content_hash, 4, 4, rhi::TextureFormat::R8G8B8A8Unorm});
    const auto second = cache.acquire(
        second_image, {second_image.content_hash, 4, 4, rhi::TextureFormat::R8G8B8A8Unorm});
    bool ok = expect(first.has_value() && second.has_value(), "both uploads succeed");
    ok = expect(cache.entry_count() == 1, "the entry bound keeps one entry") && ok;
    ok = expect(cache.stats().evictions == 1, "the older entry is evicted") && ok;

    image::ImageTextureCacheConfig tiny;
    tiny.max_bytes = 8;
    image::ImageTextureCache small_cache(device, tiny);
    const auto oversized = small_cache.acquire(
        first_image, {first_image.content_hash, 8, 8, rhi::TextureFormat::R8G8B8A8Unorm});
    ok = expect(!oversized.has_value() &&
                    oversized.error() == image::ImageTextureError::entry_too_large,
                "an entry larger than the byte bound is rejected, not uploaded") &&
         ok;

    const auto zero_extent = small_cache.acquire(
        first_image, {first_image.content_hash, 0, 8, rhi::TextureFormat::R8G8B8A8Unorm});
    const auto zero_hash =
        small_cache.acquire(first_image, {0, 4, 4, rhi::TextureFormat::R8G8B8A8Unorm});
    ok = expect(zero_extent.error() == image::ImageTextureError::invalid_request,
                "a zero extent is invalid_request") &&
         expect(zero_hash.error() == image::ImageTextureError::invalid_request,
                "an unset content hash is invalid_request") &&
         ok;
    return ok;
}

bool upload_failure_and_device_loss() {
    FakeDevice device;
    image::ImageTextureCache cache(device);
    const image::DecodedImage decoded = solid_image(2, 2, 9, 9, 9, 255);
    const image::ImageTextureKey key{decoded.content_hash, 2, 2, rhi::TextureFormat::R8G8B8A8Unorm};
    device.fail_textures = 1;
    const auto failed = cache.acquire(decoded, key);
    bool ok =
        expect(!failed.has_value() && failed.error() == image::ImageTextureError::upload_failed,
               "a texture-creation failure is upload_failed");
    ok = expect(cache.entry_count() == 0, "a failed upload is not cached") && ok;

    const auto uploaded = cache.acquire(decoded, key);
    ok = expect(uploaded.has_value() && cache.entry_count() == 1, "a retry uploads") && ok;

    device.set_status(rhi::DeviceStatus::Lost);
    const auto lost = cache.acquire(decoded, key);
    ok = expect(!lost.has_value() && lost.error() == image::ImageTextureError::device_lost,
                "a lost device is device_lost") &&
         expect(cache.entry_count() == 0, "device loss drops every texture") && ok;
    device.set_status(rhi::DeviceStatus::Ready);
    const auto recovered = cache.acquire(decoded, key);
    ok = expect(recovered.has_value() && cache.generation() == 1,
                "after recovery the generation bumps and acquire re-uploads") &&
         expect(cache.stats().invalidations >= 1, "loss events are counted") && ok;
    return ok;
}

} // namespace

bool evicted_texture_outlives_the_frame() {
    FakeDevice device;
    image::ImageTextureCacheConfig config;
    config.max_entries = 1;
    image::ImageTextureCache cache(device, config);
    const std::size_t baseline = FakeTexture::live_count;
    const auto make = [](std::uint64_t hash) {
        image::DecodedImage decoded = solid_image(4, 4, 1, 2, 3, 255);
        decoded.content_hash = hash;
        return decoded;
    };
    const image::DecodedImage a = make(0xA);
    const image::DecodedImage b = make(0xB);
    const image::DecodedImage c = make(0xC);
    const auto key = [](const image::DecodedImage& image) {
        return image::ImageTextureKey{image.content_hash, 4, 4, rhi::TextureFormat::R8G8B8A8Unorm};
    };
    const auto first = cache.acquire(a, key(a));
    const auto second = cache.acquire(b, key(b)); // evicts `a` while its draw may still be pending
    bool ok = expect(first.has_value() && second.has_value(), "both uploads succeed") &&
              expect(cache.stats().retired == 1, "the evicted texture is parked, not destroyed") &&
              expect(FakeTexture::live_count == baseline + 2,
                     "the parked texture is still alive for pending draws");
    cache.end_frame();
    ok = expect(cache.stats().retired == 0 && FakeTexture::live_count == baseline + 1,
                "end_frame destroys the parked texture") &&
         ok;

    // The parking lot is bounded: a frame that evicts more than the cache holds still releases the
    // oldest parked texture early instead of growing without limit.
    const auto third = cache.acquire(c, key(c));
    const auto again = cache.acquire(a, key(a));
    const auto more = cache.acquire(b, key(b));
    ok = expect(third.has_value() && again.has_value() && more.has_value(), "uploads succeed") &&
         expect(cache.stats().retired == 1 && cache.stats().forced_releases >= 1,
                "the parking lot stays bounded and counts forced releases") &&
         ok;
    cache.invalidate_all();
    ok = expect(FakeTexture::live_count == baseline, "invalidate_all releases parked textures") &&
         ok;
    return ok;
}

int main() {
    bool ok = true;
    ok = hit_and_upload_once() && ok;
    ok = filter_is_part_of_the_identity() && ok;
    ok = levels_are_part_of_the_identity() && ok;
    ok = resamples_to_requested_extent() && ok;
    ok = resample_is_skipped_when_the_extent_matches() && ok;
    ok = eviction_and_entry_guard() && ok;
    ok = upload_failure_and_device_loss() && ok;
    ok = evicted_texture_outlives_the_frame() && ok;
    if (!ok) {
        std::cerr << "image_texture_cache: FAILED\n";
        return 1;
    }
    std::cout << "image_texture_cache: ok\n";
    return 0;
}
