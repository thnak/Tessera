// SPDX-License-Identifier: MIT
//
// PathTextureCache contract on a fake device (no Vulkan, no GPU): hit/miss accounting, stable
// handles, deterministic LRU eviction by count and bytes, entry-too-large and invalid-request
// values, upload/rasterizer failure values, D7 device-loss generation invalidation, null-backend
// graceful behavior, and cached texture bytes equal to a direct rasterizer run for the same key.
// The checked-in pattern hash is the cross-process determinism check: any process that produces
// different bytes fails this test.

#include "path_texture_cache_test_fakes.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/path_texture_key.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using tessera::test::path_texture::expect;
using tessera::test::path_texture::fake_texture;
using tessera::test::path_texture::FakeDevice;
using tessera::test::path_texture::fnv1a;
using tessera::test::path_texture::PatternRasterizer;
using tessera::test::path_texture::triangle_path;

namespace path = tessera::rendering::path;
namespace rhi = tessera::rendering::rhi;

constexpr std::uint32_t kBox = 32;
constexpr std::uint32_t kBoxBytes = kBox * kBox * 4;

// FNV-1a of PatternRasterizer's 32x32 output. Checked in so a second process (and CI) verifies the
// same bytes; update only alongside an intentional pattern change.
constexpr std::uint64_t kExpectedPatternFnv = 0xdc7dc571b02f1b43ULL;

[[nodiscard]] path::PathTextureCacheConfig make_config(std::uint32_t max_entries,
                                                       std::uint64_t max_bytes) {
    path::PathTextureCacheConfig config;
    config.max_entries = max_entries;
    config.max_bytes = max_bytes;
    return config;
}

[[nodiscard]] path::PathTextureKey key_for(std::uint64_t id, std::uint32_t version,
                                           std::uint32_t scale_milli,
                                           const path::PathShape& shape) {
    return path::make_path_texture_key(id, version, kBox, kBox, scale_milli, shape);
}

bool hit_and_miss() {
    FakeDevice device;
    PatternRasterizer rasterizer;
    path::PathTextureCache cache(device, rasterizer, make_config(4, 1U << 20));
    const auto shape = triangle_path();
    const auto key = key_for(7, 1, 1000, shape);

    const auto first = cache.acquire(key, shape);
    bool ok = expect(first.has_value() && first->texture != nullptr,
                     "first acquire misses and uploads a texture") &&
              expect(rasterizer.calls == 1, "a miss rasterizes once") &&
              expect(device.textures_created == 1, "a miss creates one RHI texture");

    const auto second = cache.acquire(key, shape);
    ok = expect(second.has_value() && second->texture == first->texture,
                "a hit returns the same stable handle") &&
         ok;
    ok = expect(rasterizer.calls == 1 && device.textures_created == 1,
                "a hit neither rasterizes nor uploads") &&
         ok;

    const auto stats = cache.stats();
    ok = expect(stats.hits == 1 && stats.misses == 1, "stats count one hit and one miss") && ok;
    ok = expect(stats.entries == 1 && stats.bytes == kBoxBytes,
                "entry and byte accounting match the raster size") &&
         ok;
    return ok;
}

bool cached_bytes_match_direct() {
    FakeDevice device;
    PatternRasterizer rasterizer;
    path::PathTextureCache cache(device, rasterizer, make_config(4, 1U << 20));
    const auto shape = triangle_path();
    const auto cached = cache.acquire(key_for(11, 1, 1000, shape), shape);
    if (!expect(cached.has_value(), "textured acquire succeeds")) {
        return false;
    }
    const auto* texture = fake_texture(*cached);
    if (!expect(texture != nullptr, "the fake device returns a texture with readable bytes")) {
        return false;
    }

    std::vector<std::uint8_t> direct(kBoxBytes, 0);
    path::RasterTarget target{std::span<std::uint8_t>(direct), kBox, kBox, kBox * 4};
    const auto result = rasterizer.rasterize(std::span<const path::PathShape>(&shape, 1), target);
    if (!expect(result.has_value(), "direct rasterization succeeds")) {
        return false;
    }

    const bool same = texture->bytes().size() == direct.size() &&
                      std::equal(texture->bytes().begin(), texture->bytes().end(), direct.begin());
    const std::uint64_t hash = fnv1a(texture->bytes());
    std::cout << "path_texture_cache: pattern_fnv=0x" << std::hex << hash << std::dec << "\n";
    return expect(same, "uploaded texture bytes equal a direct rasterizer run") &&
           expect(hash == kExpectedPatternFnv,
                  "pattern hash matches the checked-in cross-process value");
}

bool key_sensitivity() {
    FakeDevice device;
    PatternRasterizer rasterizer;
    path::PathTextureCache cache(device, rasterizer, make_config(16, 1U << 20));
    const auto shape = triangle_path();
    const auto base = key_for(1, 1, 1000, shape);

    bool ok = expect(cache.acquire(base, shape).has_value(), "base key misses") &&
              expect(cache.acquire(base, shape).has_value() && rasterizer.calls == 1,
                     "identical key hits");

    const auto scaled = cache.acquire(key_for(1, 1, 2000, shape), shape);
    ok = expect(scaled.has_value() && scaled->extent.width == kBox * 2 &&
                    scaled->extent.height == kBox * 2,
                "scale is part of the key and rasterizes at 2x extent") &&
         ok;
    ok = expect(rasterizer.calls == 2, "a scale change misses") && ok;

    const auto version = cache.acquire(key_for(1, 2, 1000, shape), shape);
    ok = expect(version.has_value() && rasterizer.calls == 3, "a version bump misses") && ok;

    auto restyled = shape;
    restyled.paint.fill = path::Rgba8{1, 2, 3, 255};
    const auto fill = cache.acquire(key_for(1, 1, 1000, restyled), restyled);
    ok = expect(fill.has_value() && rasterizer.calls == 4, "a fill color change misses") && ok;

    auto even_odd = shape;
    even_odd.path.fill_rule = path::FillRule::even_odd;
    const auto rule = cache.acquire(key_for(1, 1, 1000, even_odd), even_odd);
    ok = expect(rule.has_value() && rasterizer.calls == 5, "a fill rule change misses") && ok;

    auto stroked = shape;
    stroked.paint.stroke = path::StrokeStyle{path::Rgba8{9, 9, 9, 255}, 2.0f, path::LineCap::round,
                                             path::LineJoin::bevel, 4.0f};
    const auto stroke = cache.acquire(key_for(1, 1, 1000, stroked), stroked);
    ok = expect(stroke.has_value() && rasterizer.calls == 6, "a stroke signature change misses") &&
         ok;
    return ok;
}

bool eviction_by_count() {
    FakeDevice device;
    PatternRasterizer rasterizer;
    path::PathTextureCache cache(device, rasterizer, make_config(2, 1U << 20));
    const auto shape = triangle_path();

    const auto k0 = key_for(100, 1, 1000, shape);
    const auto k1 = key_for(101, 1, 1000, shape);
    const auto k2 = key_for(102, 1, 1000, shape);

    bool ok = expect(cache.acquire(k0, shape).has_value(), "first key caches") &&
              expect(cache.acquire(k1, shape).has_value(), "second key caches");
    // Touching k0 makes k1 the least recently used entry.
    ok = expect(cache.acquire(k0, shape).has_value() && cache.stats().hits == 1,
                "touching k0 updates LRU order") &&
         ok;
    ok = expect(cache.acquire(k2, shape).has_value(), "third key caches") && ok;

    const auto stats = cache.stats();
    ok = expect(stats.evictions == 1 && stats.entries == 2, "count bound evicts the LRU entry") &&
         ok;
    ok = expect(rasterizer.calls == 3, "eviction does not re-rasterize") && ok;

    // k1 was evicted; re-acquiring it misses and evicts k0 (the next LRU entry).
    ok = expect(cache.acquire(k1, shape).has_value() && rasterizer.calls == 4,
                "the evicted key re-rasterizes on demand") &&
         ok;
    ok = expect(cache.stats().evictions == 2 && cache.entry_count() == 2,
                "second eviction keeps the count bound") &&
         ok;
    return ok;
}

bool eviction_by_bytes() {
    FakeDevice device;
    PatternRasterizer rasterizer;
    path::PathTextureCache cache(device, rasterizer, make_config(8, 2ULL * kBoxBytes));
    const auto shape = triangle_path();

    bool ok = cache.acquire(key_for(0, 1, 1000, shape), shape).has_value() &&
              cache.acquire(key_for(1, 1, 1000, shape), shape).has_value() &&
              cache.acquire(key_for(2, 1, 1000, shape), shape).has_value();
    ok = expect(ok && cache.entry_count() == 2, "byte bound limits entries") && ok;
    ok = expect(cache.byte_count() == 2ULL * kBoxBytes, "byte accounting stays within the bound") &&
         ok;
    ok = expect(cache.stats().evictions == 1, "byte bound evicts exactly one entry") && ok;
    return ok;
}

bool rejected_requests() {
    FakeDevice device;
    PatternRasterizer rasterizer;
    path::PathTextureCache cache(device, rasterizer, make_config(4, kBoxBytes - 1));
    const auto shape = triangle_path();

    const auto too_large = cache.acquire(key_for(1, 1, 1000, shape), shape);
    bool ok = expect(!too_large.has_value() &&
                         too_large.error() == path::PathTextureError::entry_too_large,
                     "an entry larger than max_bytes reports entry_too_large") &&
              expect(rasterizer.calls == 0, "rejected requests do not rasterize");

    path::PathTextureKey zero_width = key_for(1, 1, 1000, shape);
    zero_width.width = 0;
    ok = expect(!cache.acquire(zero_width, shape).has_value() &&
                    cache.acquire(zero_width, shape).error() ==
                        path::PathTextureError::invalid_request,
                "zero width is invalid_request") &&
         ok;

    path::PathTextureKey zero_scale = key_for(1, 1, 0, shape);
    ok = expect(!cache.acquire(zero_scale, shape).has_value() &&
                    cache.acquire(zero_scale, shape).error() ==
                        path::PathTextureError::invalid_request,
                "zero scale is invalid_request") &&
         ok;

    path::PathTextureKey huge = key_for(1, 1, 1000, shape);
    huge.width = 20000;
    ok = expect(!cache.acquire(huge, shape).has_value() &&
                    cache.acquire(huge, shape).error() == path::PathTextureError::invalid_request,
                "extent above the hard guard is invalid_request") &&
         ok;
    return ok;
}

bool device_generation_invalidation() {
    FakeDevice device;
    PatternRasterizer rasterizer;
    path::PathTextureCache cache(device, rasterizer, make_config(4, 1U << 20));
    const auto shape = triangle_path();
    const auto key = key_for(21, 1, 1000, shape);

    const auto first = cache.acquire(key, shape);
    if (!expect(first.has_value(), "texture caches on a healthy device")) {
        return false;
    }
    bool ok = expect(cache.generation() == 0 && cache.entry_count() == 1, "generation starts at 0");

    device.set_status(rhi::DeviceStatus::Lost);
    const auto lost = cache.acquire(key, shape);
    ok = expect(!lost.has_value() && lost.error() == path::PathTextureError::device_lost,
                "acquire on a lost device returns device_lost") &&
         ok;
    ok = expect(cache.entry_count() == 0 && cache.stats().invalidations == 1,
                "a lost device drops every cached texture") &&
         ok;

    device.set_status(rhi::DeviceStatus::Ready);
    const auto recovered = cache.acquire(key, shape);
    ok = expect(recovered.has_value(), "acquire after recovery re-rasterizes") && ok;
    ok = expect(cache.generation() == 1, "Lost -> Ready bumps the observed generation") && ok;
    ok = expect(rasterizer.calls == 2 && device.textures_created == 2,
                "recovery destroys the old generation and re-rasterizes/re-uploads exactly once") &&
         ok;
    return ok;
}

bool notify_device_lost() {
    FakeDevice device;
    PatternRasterizer rasterizer;
    path::PathTextureCache cache(device, rasterizer, make_config(4, 1U << 20));
    const auto shape = triangle_path();
    const auto key = key_for(22, 1, 1000, shape);

    bool ok =
        expect(cache.acquire(key, shape).has_value(), "texture caches before an explicit loss");
    cache.notify_device_lost();
    ok = expect(cache.entry_count() == 0, "notify_device_lost drops every entry") && ok;
    ok =
        expect(cache.generation() == 0, "generation bumps only once Ready is observed again") && ok;

    const auto after = cache.acquire(key, shape);
    ok = expect(after.has_value(), "acquire after notify re-rasterizes") && ok;
    ok = expect(cache.generation() == 1, "the Ready observation bumps the generation") && ok;
    return ok;
}

bool failure_values() {
    FakeDevice device;
    PatternRasterizer rasterizer;
    path::PathTextureCache cache(device, rasterizer, make_config(4, 1U << 20));
    const auto shape = triangle_path();
    const auto key = key_for(31, 1, 1000, shape);

    device.fail_textures = 1;
    const auto upload = cache.acquire(key, shape);
    bool ok = expect(!upload.has_value() && upload.error() == path::PathTextureError::upload_failed,
                     "a null texture from the device reports upload_failed") &&
              expect(cache.entry_count() == 0, "a failed upload leaves no entry");
    ok = expect(cache.acquire(key, shape).has_value(), "the next acquire retries and succeeds") &&
         ok;

    rasterizer.fail = true;
    const auto raster = cache.acquire(key_for(32, 1, 1000, shape), shape);
    ok = expect(!raster.has_value() && raster.error() == path::PathTextureError::rasterize_failed,
                "a failing rasterizer reports rasterize_failed") &&
         ok;
    rasterizer.fail = false;
    return ok;
}

bool null_backend_and_availability() {
    auto null_rasterizer = path::make_path_rasterizer(path::PathBackend::null_backend);
    if (!expect(null_rasterizer != nullptr, "the null backend is always available")) {
        return false;
    }

    FakeDevice device;
    path::PathTextureCache cache(device, *null_rasterizer, make_config(4, 1U << 20));
    const auto shape = triangle_path();
    const auto cached = cache.acquire(key_for(41, 1, 1000, shape), shape);
    bool ok =
        expect(cached.has_value(), "the null rasterizer still produces a texture") &&
        expect(fake_texture(*cached) != nullptr, "the fake device returned a readable texture");
    if (ok) {
        const auto& bytes = fake_texture(*cached)->bytes();
        ok = expect(
            std::all_of(bytes.begin(), bytes.end(), [](std::uint8_t byte) { return byte == 0; }),
            "the null rasterizer leaves a transparent texture (no crash, no pixels)");
    }

    const bool available = path::path_backend_available(path::PathBackend::thorvg);
    const auto maybe_thorvg = path::make_path_rasterizer(path::PathBackend::thorvg);
    ok = expect(available == (maybe_thorvg != nullptr),
                "backend availability matches the factory result") &&
         ok;
    return ok;
}

bool thorvg_content_equality() {
    if (!path::path_backend_available(path::PathBackend::thorvg)) {
        return expect(path::make_path_rasterizer(path::PathBackend::thorvg) == nullptr,
                      "the ThorVG backend is a graceful null when the dependency is OFF");
    }
    auto rasterizer = path::make_path_rasterizer(path::PathBackend::thorvg);
    if (!expect(rasterizer != nullptr, "the ThorVG factory returns a rasterizer")) {
        return false;
    }

    FakeDevice device;
    path::PathTextureCache cache(device, *rasterizer, make_config(4, 1U << 20));
    const auto shape = triangle_path();
    const auto cached = cache.acquire(key_for(51, 1, 1000, shape), shape);
    if (!expect(cached.has_value(), "a ThorVG raster uploads through the cache")) {
        return false;
    }
    const auto* texture = fake_texture(*cached);
    if (!expect(texture != nullptr, "the fake device returned a readable texture")) {
        return false;
    }

    std::vector<std::uint8_t> direct(kBoxBytes, 0);
    path::RasterTarget target{std::span<std::uint8_t>(direct), kBox, kBox, kBox * 4};
    const auto result = rasterizer->rasterize(std::span<const path::PathShape>(&shape, 1), target);
    if (!expect(result.has_value(), "direct ThorVG rasterization succeeds")) {
        return false;
    }
    const bool same = texture->bytes().size() == direct.size() &&
                      std::equal(texture->bytes().begin(), texture->bytes().end(), direct.begin());
    return expect(same, "ThorVG cached texture bytes equal a direct rasterizer run");
}

} // namespace

int main() {
    bool ok = true;
    ok = hit_and_miss() && ok;
    ok = cached_bytes_match_direct() && ok;
    ok = key_sensitivity() && ok;
    ok = eviction_by_count() && ok;
    ok = eviction_by_bytes() && ok;
    ok = rejected_requests() && ok;
    ok = device_generation_invalidation() && ok;
    ok = notify_device_lost() && ok;
    ok = failure_values() && ok;
    ok = null_backend_and_availability() && ok;
    ok = thorvg_content_equality() && ok;

    if (!ok) {
        std::cerr << "path_texture_cache: FAILED\n";
        return 1;
    }
    std::cout << "path_texture_cache: ok\n";
    return 0;
}
