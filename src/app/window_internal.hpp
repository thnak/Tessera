// SPDX-License-Identifier: MIT
//
// Private helpers shared by the app-window runner and the windowless frame capture: Vulkan device
// creation, the 2D pipeline/white texture, and the font/text stack a Batched2DSink needs. Not a
// public header: ui/app code sees WindowHost/capture_frame instead.

#pragma once

#include "tessera/hal/window.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/rhi/types.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/glass_blur_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_text_measurer.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_renderer.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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
    case hal::WindowSystem::NoWindow:
        break;
    }
    return rhi::SurfacePlatform::Auto;
}

// One loaded font face plus the rasterizer/shaper/TextRenderer trio. Member order matters: the
// renderer borrows the rasterizer, so the face is constructed first and never moved afterwards.
class TextStack final {
public:
    TextStack(rhi::Device& device, const std::string& font_path)
        : face(load(font_path)) {
        rasterizer = std::make_unique<ui::text::GlyphRasterizer>(face);
        shaper = std::make_unique<ui::text::HarfBuzzShaper>(face);
        renderer = std::make_unique<ui::text::TextRenderer>(
            device, *rasterizer, ui::text::GlyphAtlasConfig{1024, 1024, 2, 1});
        // Layout and paint must measure with the face that is actually drawn (issue #314): the
        // deterministic stub advance is a fixed 0.5 em per code point and does not track the font,
        // so a content-sized box would be narrower or wider than its painted ink by a
        // string-dependent amount. These two seams and the glyph renderer now share one face.
        measurer = std::make_shared<ui::text::FontTextMeasurer>(face);
        paint_metrics = std::make_unique<ui::paint::FontPaintTextMetrics>(face, kPaintMetricsId);
    }

    ~TextStack() = default;
    TextStack(const TextStack&) = delete;
    TextStack& operator=(const TextStack&) = delete;

    ui::text::FontFace face;
    std::unique_ptr<ui::text::GlyphRasterizer> rasterizer;
    std::unique_ptr<ui::text::HarfBuzzShaper> shaper;
    std::unique_ptr<ui::text::TextRenderer> renderer;
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

// Vulkan device, or null when the machine has no usable device (tests/tools then skip).
// `validation` turns the Khronos validation layer (plus synchronization validation) on for this
// device; a GPU test reads the backend's validation_message_count() to assert a run was clean.
// `surface` is the presentation family the device will be asked to build surfaces for; `Auto` keeps
// the environment-based resolution for callers that have no window.
inline std::unique_ptr<rhi::Device>
create_device(bool validation = false, rhi::SurfacePlatform surface = rhi::SurfacePlatform::Auto) {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::Vulkan;
    desc.validation = validation;
    desc.device_index = -1;
    desc.surface = surface;
    try {
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
};

inline GlassPipelines create_glass_pipelines(rhi::Device& device, rhi::TextureFormat format) {
    GlassPipelines pipelines;
    pipelines.blur = device.create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::None));
    pipelines.composite = device.create_pipeline(
        tessera::rendering::vulkan::glass_blur_pipeline_desc(format, rhi::BlendMode::Alpha));
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
