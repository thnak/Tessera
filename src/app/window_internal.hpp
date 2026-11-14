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
#include "tessera/ui/text/font_face.hpp"
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
    }

    ~TextStack() = default;
    TextStack(const TextStack&) = delete;
    TextStack& operator=(const TextStack&) = delete;

    ui::text::FontFace face;
    std::unique_ptr<ui::text::GlyphRasterizer> rasterizer;
    std::unique_ptr<ui::text::HarfBuzzShaper> shaper;
    std::unique_ptr<ui::text::TextRenderer> renderer;

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

inline std::unique_ptr<rhi::Pipeline> create_pipeline(rhi::Device& device,
                                                      rhi::TextureFormat format) {
    auto desc = tessera::rendering::vulkan::batched2d_pipeline_desc();
    desc.color_format = format;
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

// Clears to the app window background so a frame never presents uninitialized pixels. A translucent
// window clears to transparent black instead: the OS material shows through where the stylesheet
// paints nothing (glass-v1.md section 6).
inline rhi::RenderTargetDesc target_desc(rhi::Extent2D extent, rhi::TextureFormat format,
                                         rhi::Color clear_color = rhi::rgba8(11, 18, 32)) {
    rhi::RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = format;
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    desc.clear_color = clear_color;
    return desc;
}

} // namespace tessera::app::detail
