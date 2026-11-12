// SPDX-License-Identifier: MIT
//
// Private helpers shared by the app-window runner and the windowless frame capture: Vulkan device
// creation, the 2D pipeline/white texture, and the font/text stack a Batched2DSink needs. Not a
// public header: ui/app code sees WindowHost/capture_frame instead.

#pragma once

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/rendering/rhi/types.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
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
inline std::unique_ptr<rhi::Device> create_device(bool validation = false) {
    rhi::DeviceDesc desc;
    desc.backend = rhi::Backend::Vulkan;
    desc.validation = validation;
    desc.device_index = -1;
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

inline std::unique_ptr<rhi::Texture> create_white(rhi::Device& device) {
    const std::array<std::uint8_t, 4> white = {255, 255, 255, 255};
    rhi::TextureDesc desc;
    desc.extent = {1, 1};
    desc.pixels = white.data();
    desc.row_pitch = 4;
    return device.create_texture(desc);
}

// Clears to the app window background so a frame never presents uninitialized pixels.
inline rhi::RenderTargetDesc target_desc(rhi::Extent2D extent, rhi::TextureFormat format) {
    rhi::RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = format;
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    desc.clear_color = rhi::rgba8(11, 18, 32);
    return desc;
}

} // namespace tessera::app::detail
