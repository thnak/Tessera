// SPDX-License-Identifier: MIT
//
// The vector (SVG) stack one `WindowDevice` group paints through: the decoded-document cache, the
// uploaded-texture cache with its ThorVG rasterizer, and the GPU compute rasterizer that takes the
// large rasters (ADR-071). It is created against the device and the swapchain format, so it is
// shared by every window of the group and released with the other per-device caches.
//
// Frame protocol, owned by `WindowHost::Impl::render_and_present`:
//   1. paint_scene()  draws acquire textures; a GPU-routed miss is queued, not yet rasterized
//   2. flush()        submits the queued rasters in one command list, before the draw list
//   3. submit         the frame's own command list
//   4. end_frame()    collects finished rasters and destroys textures evicted while the frame ran
//
// Everything here runs on the UI thread and never waits on the GPU.

#pragma once

#include "tessera/rendering/path/gpu_path_rasterizer.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"

#include <cstdint>
#include <memory>

namespace tessera::app::detail {

namespace rhi = tessera::rendering::rhi;

class VectorStack final {
public:
    // `format` is the swapchain format: UNORM keeps the raster bytes, sRGB needs an sRGB upload so
    // sampling and target encoding round-trip. The bundles are borrowed. A build without an SVG
    // backend, no bundle, or a device that cannot create the caches leaves `available()` false and
    // the sink's vector draws stay no-ops.
    VectorStack(rhi::Device& device, rhi::TextureFormat format,
                const ui::assets::AssetBundle* assets,
                const ui::assets::ComposedAssetBundle* bundles);

    VectorStack(const VectorStack&) = delete;
    VectorStack& operator=(const VectorStack&) = delete;

    [[nodiscard]] bool available() const noexcept {
        return textures_ != nullptr;
    }
    // True when large rasters go to the GPU rasterizer (the device can run it).
    [[nodiscard]] bool gpu_route() const noexcept {
        return gpu_ != nullptr;
    }

    // The sink configuration; `device_scale` is the window's, read per frame.
    [[nodiscard]] ui::paint::VectorPaintConfig config(float device_scale);

    // Submits the queued GPU rasters. Call after the last paint draw and before the draw list.
    void flush();
    // Frame boundary. `older_frames_done` is false while a previous frame that may sample an
    // evicted texture is still running; the parked textures then wait for a later boundary instead.
    void end_frame(bool older_frames_done);

    [[nodiscard]] rendering::path::PathTextureCache::Stats stats() const;

private:
    const ui::assets::AssetBundle* assets_ = nullptr;
    const ui::assets::ComposedAssetBundle* bundles_ = nullptr;
    std::unique_ptr<rendering::path::PathRasterizer> rasterizer_;
    rendering::path::SvgDocumentCache documents_;
    // Declared before the cache that borrows it, so it is destroyed after it.
    std::unique_ptr<rendering::path::GpuPathRasterizer> gpu_;
    std::unique_ptr<rendering::path::PathTextureCache> textures_;
};

} // namespace tessera::app::detail
