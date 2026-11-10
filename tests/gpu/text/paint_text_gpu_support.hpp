// SPDX-License-Identifier: MIT
//
// Shared fixtures of the paint-text GPU tests: style stub, recording sink, render-target helpers
// and pixel counters. Split from paint_text_tests.cpp to respect the file limits.

#pragma once

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tessera::test::paint_text_gpu {

using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::StoreOp;
using tessera::rendering::rhi::TextureDesc;
using tessera::rendering::rhi::TextureFormat;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::PaintKind;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextLayout;
using tessera::ui::text::TextLine;
using tessera::ui::text::TextRenderer;
using tessera::ui::text::TextStyle;

inline constexpr Extent2D kExtent{160, 160};
inline constexpr std::uint32_t kTextRectX = 8;
inline constexpr std::uint32_t kTextRectY = 8;
inline constexpr std::uint32_t kTextRectW = 112;
inline constexpr std::uint32_t kTextRectH = 40;

inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_text_gpu: " << message << "\n";
    }
    return condition;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

inline ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

struct RecordedLine {
    std::string text;
    tessera::rendering::rhi::Rect rect;
    std::optional<float> baseline;
};

class LineRecordingSink final : public tessera::ui::paint::PaintSink {
public:
    void fill_rect(const tessera::rendering::rhi::Rect&,
                   const tessera::rendering::rhi::Color&) override {}
    void fill_rounded_rect(const tessera::rendering::rhi::Rect&,
                           const tessera::rendering::rhi::Color&, float) override {}
    void push_clip(const tessera::ui::paint::RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const tessera::ui::paint::TextDraw& run) override {
        lines.push_back(RecordedLine{std::string(run.text), run.rect, run.baseline});
    }
    std::vector<RecordedLine> lines;
};

inline RenderTargetDesc target_desc(TextureFormat format) {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.format = format;
    desc.clear_color = {0.0f, 0.0f, 0.0f, 1.0f};
    desc.load_op = LoadOp::Clear;
    desc.store_op = StoreOp::Store;
    return desc;
}

inline std::vector<std::uint8_t> read_target(Device& device, RenderTarget& target) {
    const std::size_t bytes =
        static_cast<std::size_t>(kExtent.width) * kExtent.height * sizeof(std::uint32_t);
    auto readback = device.create_buffer({bytes, BufferUsage::Readback});
    auto commands = device.create_command_list();
    commands->copy_target_to_buffer(target, *readback);
    commands->submit();
    std::vector<std::uint8_t> pixels(bytes);
    readback->read(pixels.data(), pixels.size());
    return pixels;
}

inline std::size_t count_non_background(const std::vector<std::uint8_t>& pixels) {
    std::size_t count = 0;
    for (std::size_t i = 0; i + 3 < pixels.size(); i += 4) {
        if (pixels[i] != 0 || pixels[i + 1] != 0 || pixels[i + 2] != 0) {
            ++count;
        }
    }
    return count;
}

inline std::size_t count_non_background_region(const std::vector<std::uint8_t>& pixels,
                                               std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                                               std::uint32_t y1) {
    std::size_t count = 0;
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const std::size_t index =
                (static_cast<std::size_t>(y) * kExtent.width + x) * sizeof(std::uint32_t);
            if (pixels[index] != 0 || pixels[index + 1] != 0 || pixels[index + 2] != 0) {
                ++count;
            }
        }
    }
    return count;
}

inline std::size_t count_far_right_non_background(const std::vector<std::uint8_t>& pixels) {
    std::size_t count = 0;
    for (std::uint32_t y = 0; y < kExtent.height; ++y) {
        for (std::uint32_t x = kExtent.width - 4; x < kExtent.width; ++x) {
            const std::size_t index =
                (static_cast<std::size_t>(y) * kExtent.width + x) * sizeof(std::uint32_t);
            if (pixels[index] != 0 || pixels[index + 1] != 0 || pixels[index + 2] != 0) {
                ++count;
            }
        }
    }
    return count;
}

} // namespace tessera::test::paint_text_gpu
