// SPDX-License-Identifier: MIT
//
// The PathRenderer seam (issue #685): PathTextureCache routes large misses to any renderer through
// the interface, hands it a scale-independent document, falls back to the CPU raster on a refusal,
// and never asks a renderer that says it is unavailable. A fake renderer, no GPU.

#include "path_texture_cache_test_fakes.hpp"
#include "tessera/rendering/path/gpu_path_document.hpp"
#include "tessera/rendering/path/path_renderer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/path_texture_key.hpp"

#include <cstdint>
#include <expected>
#include <iostream>
#include <memory>
#include <span>

namespace {

using tessera::test::path_texture::expect;
using tessera::test::path_texture::FakeDevice;
using tessera::test::path_texture::PatternRasterizer;
using tessera::test::path_texture::triangle_path;

namespace path = tessera::rendering::path;
namespace rhi = tessera::rendering::rhi;

[[nodiscard]] path::PathTextureCacheConfig make_config(std::uint32_t max_entries,
                                                       std::uint64_t max_bytes) {
    path::PathTextureCacheConfig config;
    config.max_entries = max_entries;
    config.max_bytes = max_bytes;
    return config;
}

// A renderer that is not the compute rasterizer: the cache only needs the PathRenderer seam.
class FakeRenderer final : public path::PathRenderer {
public:
    [[nodiscard]] bool available() const override {
        return is_available;
    }
    std::expected<void, path::PathRenderError> enqueue(std::span<const path::PathShape>,
                                                       rhi::Texture&, rhi::Extent2D,
                                                       std::uint64_t) override {
        ++shape_enqueues;
        return refuse ? std::expected<void, path::PathRenderError>(
                            std::unexpected(path::PathRenderError::too_dense))
                      : std::expected<void, path::PathRenderError>();
    }
    std::expected<void, path::PathRenderError> enqueue(std::shared_ptr<const path::GpuPathDocument>,
                                                       float, rhi::Texture&, rhi::Extent2D,
                                                       std::uint64_t) override {
        ++document_enqueues;
        return refuse ? std::expected<void, path::PathRenderError>(
                            std::unexpected(path::PathRenderError::too_dense))
                      : std::expected<void, path::PathRenderError>();
    }
    void discard_pending() override {
        ++discards;
    }
    std::expected<void, path::PathRenderError> flush() override {
        ++flushes;
        return {};
    }
    void collect() override {
        ++collects;
    }

    bool is_available = true;
    bool refuse = false;
    int shape_enqueues = 0;
    int document_enqueues = 0;
    int discards = 0;
    int flushes = 0;
    int collects = 0;
};

bool renderer_seam() {
    FakeDevice device;
    PatternRasterizer rasterizer;
    path::PathTextureCache cache(device, rasterizer, make_config(4, 1U << 22));
    FakeRenderer renderer;
    cache.set_path_renderer(&renderer);
    const auto shape = triangle_path();
    const auto key = path::make_path_texture_key(11, 1, 256, 256, 1000, shape);
    const auto result = cache.acquire(key, shape);
    bool ok = expect(result.has_value() && renderer.document_enqueues == 1 &&
                         renderer.shape_enqueues == 0 && rasterizer.calls == 0,
                     "a large miss goes to the renderer as a document, not to the CPU");
    ok = expect(cache.stats().gpu_rasterizations == 1, "and is counted as a device raster") && ok;
    ok = expect(cache.flush_gpu().has_value() && renderer.flushes >= 1,
                "flush_gpu flushes the renderer") &&
         ok;
    cache.end_frame();
    ok = expect(renderer.collects >= 1, "end_frame collects it") && ok;

    renderer.refuse = true;
    const auto other = path::make_path_texture_key(12, 1, 256, 256, 1000, shape);
    ok = expect(cache.acquire(other, shape).has_value() && rasterizer.calls == 1 &&
                    cache.stats().gpu_fallbacks == 1,
                "a refusal falls back to the CPU raster") &&
         ok;

    renderer.refuse = false;
    renderer.is_available = false;
    const auto third = path::make_path_texture_key(13, 1, 256, 256, 1000, shape);
    const int before = renderer.document_enqueues;
    ok = expect(cache.acquire(third, shape).has_value() && renderer.document_enqueues == before,
                "an unavailable renderer is not asked") &&
         ok;
    cache.set_path_renderer(nullptr);
    return ok;
}

} // namespace

int main() {
    if (!renderer_seam()) {
        std::cerr << "path_renderer_seam: FAILED\n";
        return 1;
    }
    std::cout << "path_renderer_seam: ok\n";
    return 0;
}
