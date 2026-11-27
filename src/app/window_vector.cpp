// SPDX-License-Identifier: MIT

#include "tessera/rendering/path/svg_loader.hpp"
#include "tessera/rendering/vulkan/path_raster_pipeline.hpp"
#include "window_vector.hpp"

#include <exception>

namespace tessera::app::detail {

namespace path = tessera::rendering::path;

namespace {

// Pipeline sets built at startup so the first batch does not pay for them (about 2 ms each on the
// RX 5300M). Two cover a batch in flight and the one being recorded.
constexpr std::uint32_t kWarmSlots = 2;

// A window keeps a few hundred icons resident, bounded by entries and by bytes so a gallery of
// large SVGs cannot grow the cache without limit.
constexpr std::uint32_t kMaxTextures = 256;
constexpr std::uint64_t kMaxTextureBytes = 64ULL * 1024ULL * 1024ULL;

} // namespace

VectorStack::VectorStack(rhi::Device& device, rhi::TextureFormat format,
                         const ui::assets::AssetBundle* assets,
                         const ui::assets::ComposedAssetBundle* bundles, bool allow_gpu)
    : assets_(assets),
      bundles_(bundles) {
    if (assets == nullptr && bundles == nullptr) {
        return; // nothing to draw: the sink's vector draws stay no-ops
    }
    if (!path::svg_backend_available()) {
        return;
    }
    rasterizer_ = path::make_path_rasterizer(path::PathBackend::thorvg);
    if (rasterizer_ == nullptr) {
        return;
    }
    path::PathTextureCacheConfig config;
    config.max_entries = kMaxTextures;
    config.max_bytes = kMaxTextureBytes;
    config.format = format;
    try {
        textures_ = std::make_unique<path::PathTextureCache>(device, *rasterizer_, config);
    } catch (const std::exception&) {
        textures_.reset();
        return;
    }
    if (!allow_gpu || !path::GpuPathRasterizer::supported(device)) {
        return;
    }
    try {
        // A 4K raster's output buffer is 33 MB (64 MiB as a pooled power of two); the default 16
        // MiB pool bound would destroy and recreate it every flush, so the steady state keeps one
        // (measured in gpu_path_animated_measure, ADR-071).
        path::GpuPathRasterizerConfig gpu_config;
        gpu_config.max_pooled_bytes = 96ULL * 1024ULL * 1024ULL;
        auto gpu = std::make_unique<path::GpuPathRasterizer>(
            device, tessera::rendering::vulkan::path_raster_pipeline_descs(), gpu_config);
        if (gpu->warm(kWarmSlots).has_value()) {
            gpu_ = std::move(gpu);
            textures_->set_gpu_rasterizer(gpu_.get());
        }
    } catch (const std::exception&) {
        // No pipelines on this device: every raster stays on ThorVG.
        gpu_.reset();
    }
}

ui::paint::VectorPaintConfig VectorStack::config(float device_scale) {
    ui::paint::VectorPaintConfig config;
    if (textures_ == nullptr) {
        return config;
    }
    config.bundle = assets_;
    config.bundles = bundles_;
    config.documents = &documents_;
    config.textures = textures_.get();
    config.device_scale = device_scale;
    return config;
}

void VectorStack::flush() {
    if (textures_ != nullptr) {
        (void)textures_->flush_gpu();
    }
}

void VectorStack::end_frame(bool older_frames_done) {
    if (textures_ != nullptr && older_frames_done) {
        textures_->end_frame();
    }
}

path::PathTextureCache::Stats VectorStack::stats() const {
    return textures_ != nullptr ? textures_->stats() : path::PathTextureCache::Stats{};
}

} // namespace tessera::app::detail
