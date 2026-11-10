// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/image/image_decode_cache.hpp"
#include "tessera/rendering/image/image_texture_cache.hpp"
#include "tessera/rendering/path/path_texture_cache.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <cstdint>

namespace tessera::rendering::gpu {
class Batched2DRenderer;
} // namespace tessera::rendering::gpu

namespace tessera::ui::text {
class TextRenderer;
class TextShaper;
} // namespace tessera::ui::text

namespace tessera::ui::paint {

// Vector-paint resources for Batched2DSink. All pointers are BORROWED and must outlive the sink:
// the composition root (frame/app code or a test) owns the embedded bundle and both caches.
//
// Ownership/lifetime rules:
// - `bundle` must be the generated AssetBundle for this app; names are looked up per draw.
// - `documents` caches decoded SVG documents by content hash; safe to share across sinks/frames.
// - `textures` caches uploaded document textures (one per document/size/scale). It polls the RHI
//   device for D7 loss, so the caller must call `notify_device_lost()` before `Device::recover()`
//   (the same rule as any PathTextureCache owner). Eviction parks the texture until the frame
//   owner calls `end_frame()` on the cache after the frame's commands were submitted, so a frame
//   may draw more distinct assets than the cache holds. With more than one frame in flight, call
//   `end_frame()` only once the older frames have finished (or size the cache to keep their assets
//   resident); the parking lot is bounded and overflow is destroyed early.
// - `texture_format` must match the destination color space: UNORM targets keep the raster bytes
//   (deterministic goldens), sRGB targets need R8G8B8A8Srgb so sampling/target encoding round-trips
//   (D2).
// - With any pointer null (or in a ThorVG-OFF build) vector draws become documented no-ops; the
//   rest of the paint pass is unaffected.
struct VectorPaintConfig {
    const ui::assets::AssetBundle* bundle = nullptr;
    rendering::path::SvgDocumentCache* documents = nullptr;
    rendering::path::PathTextureCache* textures = nullptr;
    float device_scale = 1.0f;
    rendering::rhi::TextureFormat texture_format = rendering::rhi::TextureFormat::R8G8B8A8Unorm;
};

// Raster-image resources for Batched2DSink (D12). Borrowed pointers, same lifetime rules as
// VectorPaintConfig: the composition root owns the bundle and both caches. `texture_format` must
// also be set on the cache config and must match the destination color space (authored sRGB images:
// UNORM for UNORM targets, sRGB for sRGB targets so sampling/target encoding round-trips).
struct ImagePaintConfig {
    const ui::assets::AssetBundle* bundle = nullptr;
    rendering::image::ImageDecodeCache* decoded = nullptr;
    rendering::image::ImageTextureCache* textures = nullptr;
    float device_scale = 1.0f;
    rendering::image::ImageLimits limits{};
    rendering::rhi::TextureFormat texture_format = rendering::rhi::TextureFormat::R8G8B8A8Unorm;
};

// Counting outcomes for raster-image draws. Missing/corrupt assets are documented no-ops.
struct ImagePaintStats {
    std::uint64_t draws = 0;
    std::uint64_t missing_assets = 0;
    std::uint64_t corrupted_assets = 0;
    std::uint64_t missing_resources = 0;
    std::uint64_t texture_failures = 0;
};

// Counting outcomes for tests/diagnostics. A vector draw either draws one textured quad or is a
// silent no-op (missing asset, unsupported/malformed asset, backend unavailable, or no resources).
struct VectorPaintStats {
    std::uint64_t draws = 0;
    std::uint64_t missing_assets = 0;
    std::uint64_t unavailable_backend = 0;
    std::uint64_t failed_assets = 0;
    std::uint64_t missing_resources = 0;
};

// Forwards paint commands to the batched 2D renderer. Deliberately not a backend adapter: the
// renderer already owns the only GPU seam (rhi::CommandList), so this header stays free of
// Vulkan/WebGPU includes.
//
// Text (D9): draw_text shapes the UTF-8 run with the supplied TextShaper and draws it through the
// existing TextRenderer/glyph-atlas path. Fonts/atlases are owned by the caller; pass both pointers
// to enable text and call `begin_text_frame()` once per painted frame (atlas frame bookkeeping).
//
// Vectors: draw_vector decodes the embedded asset through SvgDocumentCache (decode once, keyed by
// content hash), rasterizes/uploads through PathTextureCache (one texture per document/size/scale)
// and draws it as one textured quad with the resolved tint. Missing assets are a no-op (see
// VectorPaintStats); nothing throws.
class Batched2DSink final : public PaintSink {
public:
    explicit Batched2DSink(rendering::gpu::Batched2DRenderer& renderer,
                           text::TextRenderer* text_renderer = nullptr,
                           text::TextShaper* shaper = nullptr, VectorPaintConfig vector = {},
                           ImagePaintConfig image = {});

    void fill_rect(const rhi::Rect& rect, const rhi::Color& color) override;
    void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color, float radius) override;
    void fill_linear_gradient(const GradientDraw& draw) override;
    void push_clip(const RoundedClip& clip) override;
    void pop_clip() override;
    [[nodiscard]] bool supports_transform() const override {
        return true;
    }
    void push_transform(const rhi::Mat3& transform) override;
    void pop_transform() override;
    void draw_text(const TextDraw& run) override;
    void draw_vector(const VectorDraw& draw) override;
    void draw_image(const ImageDraw& draw) override;

    // Forwards to TextRenderer::begin_frame. Call once per painted frame before any draw_text so
    // atlas pages are not evicted while still referenced by this frame.
    void begin_text_frame();

    [[nodiscard]] const VectorPaintStats& vector_stats() const {
        return vector_stats_;
    }
    [[nodiscard]] const ImagePaintStats& image_stats() const {
        return image_stats_;
    }

private:
    rendering::gpu::Batched2DRenderer* renderer_ = nullptr;
    text::TextRenderer* text_renderer_ = nullptr;
    text::TextShaper* shaper_ = nullptr;
    VectorPaintConfig vector_;
    VectorPaintStats vector_stats_;
    ImagePaintConfig image_;
    ImagePaintStats image_stats_;
};

} // namespace tessera::ui::paint
