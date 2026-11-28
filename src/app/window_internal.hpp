// SPDX-License-Identifier: MIT
//
// Private helpers shared by the app-window runner and the windowless frame capture: Vulkan device
// creation, the 2D pipeline/white texture, and the font/text stack a Batched2DSink needs. Not a
// public header: ui/app code sees WindowHost/capture_frame instead.

#pragma once

#include "tessera/hal/window.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/rhi/types.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/worker_io_executor.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_text_measurer.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/text_renderer.hpp"
#include "tessera/ui/text/weighted_shaper.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tessera::app::detail {

namespace rhi = tessera::rendering::rhi;

// Maps a HAL window family onto the RHI presentation family a device must be created for (issue
// #155). A Linux build can present through both Xlib and Wayland; the HAL window knows which one
// this session uses, so the device never has to guess from the environment.
inline rhi::SurfacePlatform surface_platform_for(hal::WindowSystem window_system) {
    switch (window_system) {
    case hal::WindowSystem::Win32:
        return rhi::SurfacePlatform::Win32;
    case hal::WindowSystem::Xlib:
        return rhi::SurfacePlatform::Xlib;
    case hal::WindowSystem::Wayland:
        return rhi::SurfacePlatform::Wayland;
    case hal::WindowSystem::Android:
        return rhi::SurfacePlatform::Android;
    case hal::WindowSystem::NoWindow:
        break;
    }
    return rhi::SurfacePlatform::Auto;
}

// The loaded font faces plus the rasterizer/shaper/TextRenderer trio. `font_path` is the primary
// face; each `extra_font_paths` entry is another face of the family (a bold file, say), picked per
// run by CSS `font-weight` from the weight each file declares (`WeightedShaper`). Member order
// matters: the renderer borrows the rasterizers, so the faces are constructed first and never moved
// afterwards.
class TextStack final {
public:
    TextStack(rhi::Device& device, const std::string& font_path,
              const std::vector<std::string>& extra_font_paths = {})
        : face(load(font_path)) {
        faces.push_back(face);
        for (const std::string& extra : extra_font_paths) {
            faces.push_back(load(extra));
        }
        std::vector<ui::text::GlyphRasterizer*> raster_stack;
        rasterizers.reserve(faces.size());
        for (const ui::text::FontFace& loaded : faces) {
            rasterizers.push_back(std::make_unique<ui::text::GlyphRasterizer>(loaded));
            raster_stack.push_back(rasterizers.back().get());
        }
        rasterizer = rasterizers.front().get();
        shaper = std::make_unique<ui::text::WeightedShaper>(faces);
        renderer = std::make_unique<ui::text::TextRenderer>(
            device, std::move(raster_stack), ui::text::GlyphAtlasConfig{1024, 1024, 2, 1});
        // Layout and paint must measure with the face that is actually drawn (issue #314): the
        // deterministic stub advance is a fixed 0.5 em per code point and does not track the font,
        // so a content-sized box would be narrower or wider than its painted ink by a
        // string-dependent amount. These two seams and the glyph renderer now share one face.
        // One in-flight step and one worker: a newer step abandons the older one, so a second slot
        // would only hold a job nobody installs.
        scale_executor = std::make_unique<runtime::WorkerIoExecutor>(
            scale_completions, runtime::WorkerIoExecutor::Config{1, 1, 4});
        measurer = std::make_shared<ui::text::FontTextMeasurer>(faces);
        paint_metrics = std::make_unique<ui::paint::FontPaintTextMetrics>(faces, kPaintMetricsId);
    }

    // Abandons a pending step first so the join in the executor's destructor ends at the worker's
    // next glyph instead of after the full rasterization.
    // Applies `policy` without rasterizing on the calling thread; falls back to the synchronous
    // path only when the worker refuses the step.
    void apply_policy(const ui::scale::AaPolicy& policy) {
        if (!renderer->begin_aa_policy_step(policy, *scale_executor)) {
            renderer->set_aa_policy(policy);
        }
    }

    // Between frames: runs the worker's completion (bounded, never waits) and lands a finished
    // step. True when the active policy changed, which stales every quad batched at the old scale.
    // `pending` reports a step still in flight so the loop keeps ticking until it lands.
    bool poll_scale_step(bool& pending) {
        static_cast<void>(scale_completions.run(4));
        const bool landed = renderer->poll_render_scale_step();
        pending = renderer->render_scale_step_pending();
        return landed;
    }

    ~TextStack() {
        if (renderer != nullptr) {
            renderer->cancel_render_scale_step();
        }
        scale_executor.reset();
    }
    TextStack(const TextStack&) = delete;
    TextStack& operator=(const TextStack&) = delete;

    ui::text::FontFace face;
    std::vector<ui::text::FontFace> faces; // primary first, then the extras
    std::vector<std::unique_ptr<ui::text::GlyphRasterizer>> rasterizers;
    ui::text::GlyphRasterizer* rasterizer = nullptr; // the primary face's
    std::unique_ptr<ui::text::WeightedShaper> shaper;
    std::unique_ptr<ui::text::TextRenderer> renderer;
    // The off-thread glyph scale step (issue #993). The queue and executor are declared after the
    // renderer so they die first: the executor joins its worker (no detached tasks) while the faces
    // and the renderer it prepares glyphs for are still alive, and the queue outlives the executor
    // that posts to it.
    runtime::CompletionQueue scale_completions{16};
    std::unique_ptr<runtime::WorkerIoExecutor> scale_executor;
    // The same face as a layout measurer and as paint's wrapping metrics source (issue #314): the
    // stub advance is a fixed 0.5 em per code point and does not track the font, so a content-sized
    // box disagrees with its painted ink by a string-dependent amount. A caller installs both
    // (HostConfig::text_measurer and PaintContext::text_metrics_source).
    //
    // Lifetime: both objects own their own `FontFace` copy, and `FontFace` holds a
    // `shared_ptr<FontFaceState>` - so the measurer keeps the FreeType face alive on its own and
    // the layout engine's `shared_ptr` to it is never dangling, even though `TextStack` is
    // destroyed (or replaced on device loss) after the `AppHost` that borrows the measurer. The
    // glyphs the sink draws come from `renderer`/`shaper` above, which live exactly as long as this
    // object and the sink that borrows them.
    std::shared_ptr<const ui::text::TextMeasurer> measurer;
    std::unique_ptr<ui::paint::FontPaintTextMetrics> paint_metrics;

    // Stable per-face id for the paint text-layout cache key; distinct from the stub's id 0 so stub
    // and real-face line boxes can never mix in one cache.
    static constexpr std::uint32_t kPaintMetricsId = 1;

private:
    static ui::text::FontFace load(const std::string& path) {
        if (path.empty()) {
            throw std::invalid_argument("tessera app window: a font path is required");
        }
        auto loaded = ui::text::FontFace::load_from_file(path);
        if (!loaded) {
            throw std::runtime_error("tessera app window: failed to load font " + path + ": " +
                                     loaded.error().message);
        }
        return *loaded;
    }
};

// The measurer a host's layout engine should use: the caller's explicit one wins, otherwise the
// presentation's own real-face measurer. Layout and the glyph renderer then share one face (#314).
inline std::shared_ptr<const ui::text::TextMeasurer>
layout_measurer(const std::shared_ptr<const ui::text::TextMeasurer>& requested,
                const TextStack& text) {
    return requested != nullptr ? requested : text.measurer;
}

// The RHI device, or null when the machine has no usable device (tests/tools then skip).
// `validation` turns the Khronos validation layer (plus synchronization validation) on for this
// device; a GPU test reads the backend's validation_message_count() to assert a run was clean.
// `surface` is the presentation family the device will be asked to build surfaces for; `Auto` keeps
// the environment-based resolution for callers that have no window.
//
// `backend` selects the RHI implementation (issue #228). `Vulkan` is production. `Null` is the
// deterministic backend the multi-window acceptance runs on: it records commands instead of
// presenting, so it needs no surface and works on a headless lane, while the platform window, the
// input path and the metrics path are still real. The pipeline factories below are Vulkan-specific,
// so a null-backend group reports `available()` and then fails `ensure_presentation` with a
// diagnostic rather than pretending to present; the multi-window test therefore exercises the
// *device sharing* half on the null backend and the *presentation* half on a real one.
inline std::unique_ptr<rhi::Device>
create_device(bool validation = false, rhi::SurfacePlatform surface = rhi::SurfacePlatform::Auto,
              rhi::Backend backend = rhi::Backend::Vulkan) {
    rhi::DeviceDesc desc;
    desc.backend = backend;
    desc.validation = validation;
    desc.device_index = -1;
    desc.surface = surface;
    try {
        if (backend == rhi::Backend::Null) {
            return tessera::rendering::null::create_null_device(desc);
        }
        return tessera::rendering::vulkan::create_vulkan_device(desc);
    } catch (const std::exception&) {
        return nullptr;
    }
}

inline std::unique_ptr<rhi::Pipeline>
create_pipeline(rhi::Device& device, rhi::TextureFormat format, std::uint32_t sample_count = 1) {
    auto desc = tessera::rendering::vulkan::batched2d_pipeline_desc(format, sample_count);
    desc.color_format = format;
    return device.create_pipeline(desc);
}

// The dual-source LCD text pipeline (display-aware-rendering.md section 3.4.1). Null when the
// device cannot blend per channel or refuses the pipeline; the window then resolves grayscale text.
inline std::unique_ptr<rhi::Pipeline>
create_lcd_pipeline(rhi::Device& device, rhi::TextureFormat format, std::uint32_t sample_count) {
    if (!device.supports_dual_source_blend()) {
        return nullptr;
    }
    auto desc = tessera::rendering::vulkan::batched2d_lcd_pipeline_desc(format, sample_count);
    desc.color_format = format;
    return device.create_pipeline(desc);
}

// The single-sample pipeline that presents a 4x frame (S8, display-aware-rendering section 4): the
// same batched2d shader and vertex layout, one sample, and no blending. No blending matters: the
// present quad replaces the frame target with the resolved image's own encoded pixels, so the
// result equals what a direct 1-sample frame would have written (a second alpha composite over the
// clear would darken a translucent frame). Created only when the presentation resolves to 4x.
inline std::unique_ptr<rhi::Pipeline> create_present_pipeline(rhi::Device& device,
                                                              rhi::TextureFormat format) {
    auto desc = tessera::rendering::vulkan::batched2d_pipeline_desc(format, 1);
    desc.blend = rhi::BlendMode::None;
    return device.create_pipeline(desc);
}

// The two glass pipelines a backdrop composite records (issue #244): the horizontal blur draws into
// a pooled offscreen target with no blending, the composite draws over the frame target with alpha
// blending so the rounded clip antialiases and the node opacity folds in. Both are created for the
// frame target's format, like the 2D pipeline.
struct GlassPipelines {
    std::unique_ptr<rhi::Pipeline> blur;
    std::unique_ptr<rhi::Pipeline> composite;
    // The layer mask step (RHI 1.19, issue #617): optional, so a device that cannot create it only
    // loses `mask-image`, never the window.
    std::unique_ptr<rhi::Pipeline> mask;
    // The combine steps of a `mask-composite` layer (RHI 1.21); optional like `mask`.
    std::unique_ptr<rhi::Pipeline> mask_out;
    std::unique_ptr<rhi::Pipeline> mask_xor;
};

inline GlassPipelines create_glass_pipelines(rhi::Device& device, rhi::TextureFormat format) {
    GlassPipelines pipelines;
    pipelines.blur = device.create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::None));
    pipelines.composite = device.create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::Alpha));
    try {
        pipelines.mask =
            device.create_pipeline(tessera::rendering::vulkan::glass_blur_pipeline_desc(
                format, rhi::BlendMode::DestinationIn));
    } catch (const std::exception&) {
        pipelines.mask.reset();
    }
    const auto optional_blend = [&](rhi::BlendMode blend) -> std::unique_ptr<rhi::Pipeline> {
        try {
            return device.create_pipeline(
                tessera::rendering::vulkan::glass_blur_pipeline_desc(format, blend));
        } catch (const std::exception&) {
            return nullptr;
        }
    };
    pipelines.mask_out = optional_blend(rhi::BlendMode::SourceOut);
    pipelines.mask_xor = optional_blend(rhi::BlendMode::Xor);
    return pipelines;
}

inline std::unique_ptr<rhi::Texture> create_white(rhi::Device& device) {
    const std::array<std::uint8_t, 4> white = {255, 255, 255, 255};
    rhi::TextureDesc desc;
    desc.extent = {1, 1};
    desc.pixels = white.data();
    desc.row_pitch = 4;
    return device.create_texture(desc);
}

// The frame clear color. A translucent window clears to transparent black so the OS material shows
// through where the stylesheet paints nothing (glass-v1.md section 6); every other window clears to
// the canvas colour, white as a browser's default canvas is (ADR-063), so a frame never presents
// uninitialized pixels. An author paints the window with a background on the root.
inline rhi::Color frame_clear_color(bool translucent_presentation) {
    return translucent_presentation ? rhi::rgba8(0, 0, 0, 0) : rhi::rgba8(255, 255, 255);
}

// Clears to the app window background so a frame never presents uninitialized pixels. A translucent
// window clears to transparent black instead: the OS material shows through where the stylesheet
// paints nothing (glass-v1.md section 6).
inline rhi::RenderTargetDesc target_desc(rhi::Extent2D extent, rhi::TextureFormat format,
                                         rhi::Color clear_color = rhi::rgba8(255, 255, 255),
                                         std::uint32_t sample_count = 1) {
    rhi::RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = format;
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    desc.clear_color = clear_color;
    desc.sample_count = sample_count;
    return desc;
}

} // namespace tessera::app::detail
