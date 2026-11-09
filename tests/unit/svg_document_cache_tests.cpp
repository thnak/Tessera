// SPDX-License-Identifier: MIT
//
// Decoded-SVG document cache contract: content-key stability, decode-once-then-hit, deterministic
// LRU eviction by entry count and by total shape/segment budget, invalidation, failure values that
// are never cached, the ThorVG-OFF `backend_unavailable` contract, and the document -> texture path
// (one texture per document/size/scale, equal to a direct decode+rasterize, deterministic bytes).

#include "path_texture_cache_test_fakes.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/path_texture_key.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/path/svg_loader.hpp"
#include "tessera/rendering/path/svg_texture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace path = tessera::rendering::path;

using tessera::test::path_texture::fake_texture;
using tessera::test::path_texture::FakeDevice;
using tessera::test::path_texture::fnv1a;

// FNV-1a 64 of the icon_shapes.svg bytes; checked in so key stability is verified across runs,
// processes and working directories (the hash must not depend on anything but the content).
constexpr std::uint64_t kIconShapesKeyHash = 0x14bc863136cb38d9ULL;
// FNV-1a 64 of the document texture bytes at 1x and 2x (64x64 and 128x128) for icon_shapes.svg.
constexpr std::uint64_t kIconTextureFnv1x = 0xa0172eea2bc1df50ULL;
constexpr std::uint64_t kIconTextureFnv2x = 0xa06d760467d58f2fULL;

constexpr std::uint32_t kIconSize = 64;

[[nodiscard]] bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "svg_document_cache: " << message << "\n";
    }
    return condition;
}

[[nodiscard]] std::vector<std::uint8_t> read_fixture(std::string_view name) {
    const std::string file = std::string(TESSERA_SVG_FIXTURE_DIR) + "/" + std::string(name);
    std::ifstream input(file, std::ios::binary);
    if (!input) {
        std::cerr << "svg_document_cache: cannot open fixture " << file << "\n";
        return {};
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>());
}

[[nodiscard]] path::SvgDocumentCacheConfig
make_config(std::uint32_t max_entries, std::uint32_t max_shapes, std::uint32_t max_segments) {
    path::SvgDocumentCacheConfig config;
    config.max_entries = max_entries;
    config.max_shapes = max_shapes;
    config.max_segments = max_segments;
    return config;
}

bool key_stability() {
    const auto bytes = read_fixture("icon_shapes.svg");
    const auto other = read_fixture("stroke_path.svg");
    if (!expect(!bytes.empty() && !other.empty(), "fixtures are readable")) {
        return false;
    }
    const path::SvgDocumentKey first = path::make_svg_document_key(bytes);
    const path::SvgDocumentKey second = path::make_svg_document_key(bytes);
    std::cout << "svg_document_cache: icon_shapes_key_hash=0x" << std::hex << first.content_hash
              << std::dec << "\n";

    path::SvgLimits strict;
    strict.max_shapes = 1;
    const path::SvgDocumentKey strict_key = path::make_svg_document_key(bytes, strict);
    return expect(first == second, "the same bytes produce the same key") &&
           expect(first.content_hash == kIconShapesKeyHash,
                  "the content hash matches the checked-in cross-process value") &&
           expect(first.content_hash != path::make_svg_document_key(other).content_hash,
                  "different bytes produce a different key") &&
           expect(!(first == strict_key), "different decode limits produce a different key");
}

bool offline_contract() {
    const auto bytes = read_fixture("icon_shapes.svg");
    if (!expect(!bytes.empty(), "fixture is readable")) {
        return false;
    }
    path::SvgDocumentCache cache;
    const auto first = cache.acquire(std::span<const std::uint8_t>(bytes));
    const auto second = cache.acquire(std::span<const std::uint8_t>(bytes));
    return expect(!first.has_value() && first.error() == path::SvgError::backend_unavailable,
                  "ThorVG OFF returns backend_unavailable instead of a link error") &&
           expect(!second.has_value() && second.error() == path::SvgError::backend_unavailable,
                  "the OFF failure repeats as a value") &&
           expect(cache.entry_count() == 0 && cache.stats().decodes == 2,
                  "OFF results are never cached");
}

bool decode_once_then_hit() {
    const auto bytes = read_fixture("icon_shapes.svg");
    if (!expect(!bytes.empty(), "fixture is readable")) {
        return false;
    }
    path::SvgDocumentCache cache;
    const auto first = cache.acquire(std::span<const std::uint8_t>(bytes));
    const auto second = cache.acquire(std::span<const std::uint8_t>(bytes));
    if (!expect(first.has_value() && second.has_value(), "valid fixture decodes")) {
        return false;
    }
    const auto stats = cache.stats();
    return expect(cache.entry_count() == 1, "one entry after two acquires") &&
           expect(stats.decodes == 1 && stats.hits == 1 && stats.misses == 1,
                  "the document is decoded once and then served from the cache") &&
           expect(first.value() == second.value(), "the document handle is stable across hits") &&
           expect(second.value()->shapes.size() == 2, "the cached document keeps its shapes");
}

bool failures_are_not_cached() {
    const auto malformed = read_fixture("malformed.svg");
    const auto unsupported = read_fixture("unsupported_text.svg");
    if (!expect(!malformed.empty() && !unsupported.empty(), "failure fixtures are readable")) {
        return false;
    }

    path::SvgDocumentCache malformed_cache;
    const auto first = malformed_cache.acquire(std::span<const std::uint8_t>(malformed));
    const auto second = malformed_cache.acquire(std::span<const std::uint8_t>(malformed));
    bool ok = expect(!first.has_value() && first.error() == path::SvgError::malformed,
                     "malformed input reports malformed") &&
              expect(!second.has_value() && second.error() == path::SvgError::malformed,
                     "the malformed failure repeats") &&
              expect(malformed_cache.entry_count() == 0 && malformed_cache.stats().decodes == 2,
                     "a failed decode is never cached (it decodes again)");

    path::SvgDocumentCache unsupported_cache;
    const auto text = unsupported_cache.acquire(std::span<const std::uint8_t>(unsupported));
    ok = expect(!text.has_value() && text.error() == path::SvgError::unsupported,
                "unsupported content reports unsupported") &&
         expect(unsupported_cache.entry_count() == 0, "unsupported content leaves no entry") && ok;

    path::SvgDocumentCache empty_cache;
    const auto empty = empty_cache.acquire(std::span<const std::uint8_t>());
    ok = expect(!empty.has_value() && empty.error() == path::SvgError::invalid_argument,
                "empty input reports invalid_argument") &&
         expect(empty_cache.entry_count() == 0, "empty input leaves no entry") && ok;
    return ok;
}

bool eviction_by_count() {
    const auto icon = read_fixture("icon_shapes.svg");
    const auto stroke = read_fixture("stroke_path.svg");
    const auto group = read_fixture("group_transform.svg");
    if (!expect(!icon.empty() && !stroke.empty() && !group.empty(), "fixtures are readable")) {
        return false;
    }
    path::SvgDocumentCache cache(make_config(2, 4096, 131072));
    bool ok = cache.acquire(std::span<const std::uint8_t>(icon)).has_value() &&
              cache.acquire(std::span<const std::uint8_t>(stroke)).has_value() &&
              cache.acquire(std::span<const std::uint8_t>(group)).has_value();
    ok = expect(ok && cache.entry_count() == 2 && cache.stats().evictions == 1,
                "count bound evicts exactly the least recently used document") &&
         ok;
    // icon_shapes was the LRU entry and is gone: acquiring it again must decode.
    ok = expect(cache.acquire(std::span<const std::uint8_t>(icon)).has_value() &&
                    cache.stats().decodes == 4,
                "the evicted document decodes again on demand") &&
         ok;
    return ok;
}

bool eviction_by_budget() {
    const auto icon = read_fixture("icon_shapes.svg");
    const auto stroke = read_fixture("stroke_path.svg");
    if (!expect(!icon.empty() && !stroke.empty(), "fixtures are readable")) {
        return false;
    }

    // Shape budget: icon_shapes has 2 shapes, stroke_path has 1.
    path::SvgDocumentCache shapes_cache(make_config(8, 2, 131072));
    bool ok = shapes_cache.acquire(std::span<const std::uint8_t>(icon)).has_value() &&
              shapes_cache.acquire(std::span<const std::uint8_t>(stroke)).has_value();
    ok = expect(ok && shapes_cache.entry_count() == 1 && shapes_cache.shape_count() == 1,
                "shape budget evicts the LRU entry") &&
         expect(shapes_cache.stats().evictions == 1, "shape-budget eviction is counted") && ok;
    ok = expect(shapes_cache.acquire(std::span<const std::uint8_t>(icon)).has_value() &&
                    shapes_cache.shape_count() == 2,
                "re-acquiring the evicted document restores the shape budget") &&
         ok;

    // Segment budget: icon_shapes has 11 segments, stroke_path has 4.
    path::SvgDocumentCache segments_cache(make_config(8, 4096, 12));
    ok = segments_cache.acquire(std::span<const std::uint8_t>(icon)).has_value() &&
         segments_cache.acquire(std::span<const std::uint8_t>(stroke)).has_value() && ok;
    ok = expect(segments_cache.entry_count() == 1 && segments_cache.segment_count() == 4,
                "segment budget evicts the LRU entry") &&
         ok;

    // A single document above the whole budget is rejected, not cached.
    path::SvgDocumentCache small_cache(make_config(8, 4096, 3));
    const auto rejected = small_cache.acquire(std::span<const std::uint8_t>(stroke));
    ok = expect(!rejected.has_value() && rejected.error() == path::SvgError::limit_exceeded,
                "a document above the total budget reports limit_exceeded") &&
         expect(small_cache.entry_count() == 0, "the over-budget document is not cached") &&
         expect(small_cache.acquire(std::span<const std::uint8_t>(stroke)).error() ==
                    path::SvgError::limit_exceeded,
                "the over-budget request fails again (no partial entry)") &&
         ok;
    return ok;
}

bool invalidation() {
    const auto bytes = read_fixture("icon_shapes.svg");
    if (!expect(!bytes.empty(), "fixture is readable")) {
        return false;
    }
    path::SvgDocumentCache cache;
    const auto first = cache.acquire(std::span<const std::uint8_t>(bytes));
    if (!expect(first.has_value(), "fixture decodes")) {
        return false;
    }
    const path::SvgDocumentKey key = path::make_svg_document_key(bytes);
    bool ok = expect(cache.invalidate(key) == 1 && cache.entry_count() == 0,
                     "invalidate(key) drops exactly that entry") &&
              expect(cache.invalidate(key) == 0, "invalidating a missing key is a no-op");
    ok = expect(cache.acquire(std::span<const std::uint8_t>(bytes)).has_value() &&
                    cache.stats().decodes == 2,
                "the invalidated document decodes again") &&
         ok;
    ok = expect(cache.invalidate_all() == 1 && cache.entry_count() == 0,
                "invalidate_all reports and drops the entry") &&
         ok;
    return ok;
}

bool document_texture_path() {
    const auto bytes = read_fixture("icon_shapes.svg");
    if (!expect(!bytes.empty(), "fixture is readable")) {
        return false;
    }
    path::SvgDocumentCache documents;
    const auto document = documents.acquire(std::span<const std::uint8_t>(bytes));
    if (!expect(document.has_value(), "fixture decodes for the texture path")) {
        return false;
    }
    auto rasterizer = path::make_path_rasterizer(path::PathBackend::thorvg);
    if (!expect(rasterizer != nullptr, "ThorVG rasterizer is available")) {
        return false;
    }

    FakeDevice device;
    path::PathTextureCache textures(device, *rasterizer, {});
    const path::SvgTextureKey key = path::make_svg_texture_key(7, 1, *document.value());
    const auto first = path::acquire_svg_texture(textures, key, *document.value());
    if (!expect(first.has_value() && first->texture != nullptr &&
                    first->extent.width == kIconSize && first->extent.height == kIconSize,
                "the document is rasterized into a 64x64 texture")) {
        return false;
    }
    const auto* texture = fake_texture(*first);
    if (!expect(texture != nullptr, "the fake device returns readable bytes")) {
        return false;
    }

    std::vector<std::uint8_t> direct(static_cast<std::size_t>(kIconSize) * kIconSize * 4U, 0);
    path::RasterTarget target{std::span<std::uint8_t>(direct), kIconSize, kIconSize,
                              kIconSize * 4U};
    const auto raster =
        rasterizer->rasterize(std::span<const path::PathShape>(document.value()->shapes), target);
    if (!expect(raster.has_value(), "direct document rasterization succeeds")) {
        return false;
    }
    const std::uint64_t hash_1x = fnv1a(texture->bytes());
    std::cout << "svg_document_cache: icon_texture_fnv_1x=0x" << std::hex << hash_1x << std::dec
              << "\n";
    bool ok = expect(texture->bytes() == direct,
                     "the document texture equals a direct decode+rasterize") &&
              expect(hash_1x == kIconTextureFnv1x,
                     "1x document texture matches the checked-in cross-process FNV");

    const auto second = path::acquire_svg_texture(textures, key, *document.value());
    ok = expect(second.has_value() && second->texture == first->texture &&
                    textures.stats().uploads == 1 && textures.stats().hits == 1,
                "the second document acquire is a cache hit") &&
         ok;

    const path::SvgTextureKey key_2x = path::make_svg_texture_key(7, 1, *document.value(), 2000);
    const auto scaled = path::acquire_svg_texture(textures, key_2x, *document.value());
    if (ok && expect(scaled.has_value() && scaled->extent.width == kIconSize * 2 &&
                         scaled->extent.height == kIconSize * 2,
                     "2x document texture is 128x128")) {
        const auto* scaled_texture = fake_texture(*scaled);
        const std::uint64_t hash_2x = fnv1a(scaled_texture->bytes());
        std::cout << "svg_document_cache: icon_texture_fnv_2x=0x" << std::hex << hash_2x << std::dec
                  << "\n";
        ok = expect(hash_2x == kIconTextureFnv2x,
                    "2x document texture matches the checked-in cross-process FNV") &&
             ok;
    } else {
        ok = false;
    }

    // A single-shape key with the same id/version/size/scale must not hit the document entry.
    path::PathTextureKey single;
    single.path_id = 7;
    single.path_version = 1;
    single.width = kIconSize;
    single.height = kIconSize;
    single.scale_milli = 1000;
    const std::uint64_t uploads_before = textures.stats().uploads;
    const auto single_result = textures.acquire(single, document.value()->shapes.front());
    ok = expect(single_result.has_value() && textures.stats().uploads == uploads_before + 1,
                "document and single-shape texture namespaces stay disjoint") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = key_stability() && ok;
    if (!path::svg_backend_available()) {
        ok = offline_contract() && ok;
        std::cout << "svg_document_cache: ThorVG OFF, online cache checks skipped\n";
    } else {
        ok = decode_once_then_hit() && ok;
        ok = failures_are_not_cached() && ok;
        ok = eviction_by_count() && ok;
        ok = eviction_by_budget() && ok;
        ok = invalidation() && ok;
        ok = document_texture_path() && ok;
    }
    if (!ok) {
        std::cerr << "svg_document_cache: FAILED\n";
        return 1;
    }
    std::cout << "svg_document_cache: ok\n";
    return 0;
}
