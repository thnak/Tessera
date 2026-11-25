// SPDX-License-Identifier: MIT
//
// Shared scaffolding for the shaped-clip readback tests (ADR-068 slices 5 and 6): a real Vulkan
// device and renderer in golden mode (AA off, UNORM target), and a scene runner that paints one
// full-target quad under a stack of `clip-path` values and compares every pixel with the CPU
// reference (`CpuCanvas::fill_rect_clipped`, which uses `gpu::clip::covers_pixel`).

#pragma once

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/clip_shapes.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/rendering/vulkan/batched2d_pipeline.hpp"
#include "tessera/rendering/vulkan/vulkan_device.hpp"
#include "tessera/ui/paint/clip_path.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tessera::test::clipshape {

namespace clip = rendering::gpu::clip;
namespace detail = ui::paint::detail;
namespace rhi = rendering::rhi;

inline constexpr rhi::Extent2D kExtent{64, 64};
inline constexpr rhi::Rect kBox{0.0f, 0.0f, 64.0f, 64.0f};
inline constexpr rhi::Color kRed = rhi::rgba8(255, 0, 0);

inline bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "clip_shape: " << message << "\n";
    }
    return condition;
}

// One scene: the shapes pushed outermost first, each a `clip-path` value parsed by the same parser
// the engine will use.
struct Scene {
    std::string name;
    std::vector<std::string> shapes;
    // True when a plain 16 px rounded rect is pushed under the shapes.
    bool rounded_outer = false;
    std::uint32_t expected_fallbacks = 0;
};

struct Outcome {
    std::size_t mismatches = 0;
    std::size_t painted = 0;
    std::uint32_t fallbacks = 0;
};

// A Vulkan device, pipeline and renderer, or nothing when the host has no Vulkan.
class Fixture {
public:
    static std::unique_ptr<Fixture> create(const char* test_name) {
        rhi::DeviceDesc device_desc;
        device_desc.backend = rhi::Backend::Vulkan;
        device_desc.validation = true;
        auto fixture = std::unique_ptr<Fixture>(new Fixture);
        try {
            fixture->device_ = rendering::vulkan::create_vulkan_device(device_desc);
        } catch (const std::exception& error) {
            std::cout << test_name << ": skipped (Vulkan device unavailable: " << error.what()
                      << ")\n";
            return nullptr;
        }
        rhi::RenderTargetDesc& desc = fixture->desc_;
        desc.extent = kExtent;
        desc.clear_color = rhi::Color{0.0f, 0.0f, 0.0f, 1.0f};
        desc.load_op = rhi::LoadOp::Clear;
        desc.store_op = rhi::StoreOp::Store;
        fixture->target_ = fixture->device_->create_render_target(desc);
        fixture->pipeline_ =
            fixture->device_->create_pipeline(rendering::vulkan::batched2d_pipeline_desc());
        const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
        rhi::TextureDesc white_desc;
        white_desc.extent = {1, 1};
        white_desc.pixels = white_pixel.data();
        white_desc.row_pitch = 4;
        fixture->white_ = fixture->device_->create_texture(white_desc);
        // AA off + golden mode: hard edges and exact bytes.
        fixture->renderer_ = std::make_unique<rendering::gpu::Batched2DRenderer>(
            *fixture->device_, *fixture->pipeline_, *fixture->white_,
            rendering::gpu::Batched2DConfig{4096, false, true});
        return fixture;
    }

    // Runs `scene` on the GPU and on the CPU reference.
    Outcome run(const Scene& scene) {
        std::vector<clip::ClipRecord> records;
        std::vector<clip::ClipVertex> pool;
        std::vector<detail::ClipShape> parsed;
        for (const std::string& css : scene.shapes) {
            parsed.push_back(detail::parse_clip_path(css, kBox).shape);
        }

        auto commands = device_->create_command_list();
        renderer_->begin(*commands, *target_, desc_);
        std::size_t depth = 0;
        if (scene.rounded_outer) {
            const clip::RoundedRect outer = clip::uniform_rect({8.0f, 8.0f, 48.0f, 48.0f}, 16.0f);
            renderer_->push_clip(outer);
            ++depth;
            clip::ClipRecord record;
            record.kind = static_cast<std::uint32_t>(clip::RecordKind::RoundedRect);
            record.x = outer.rect.x;
            record.y = outer.rect.y;
            record.width = outer.rect.width;
            record.height = outer.rect.height;
            record.radii[0] = record.radii[1] = record.radii[2] = record.radii[3] = 16.0f;
            records.push_back(record);
        }
        for (const detail::ClipShape& shape : parsed) {
            clip::ClipRecord record = detail::to_clip_record(shape, kBox);
            clip::ShapeClip shaped;
            shaped.record = record;
            shaped.vertices = shape.vertices;
            renderer_->push_clip_shape(shaped);
            ++depth;
            // The CPU reference sees the pool the table would build: this scene's polygons in
            // order (no scene repeats a polygon, so there is nothing to deduplicate).
            if (record.kind == static_cast<std::uint32_t>(clip::RecordKind::Polygon)) {
                if (shape.vertices.size() > clip::kMaxPolygonVertices) {
                    // Over the limit the table degrades to the bounding box: same rect, no polygon.
                    record.kind = static_cast<std::uint32_t>(clip::RecordKind::RoundedRect);
                    record.vertex_count = 0;
                } else {
                    record.first_vertex = static_cast<std::uint32_t>(pool.size());
                    pool.insert(pool.end(), shape.vertices.begin(), shape.vertices.end());
                }
            }
            records.push_back(record);
        }
        renderer_->push_rect({0, 0, 64, 64}, kRed);
        for (; depth > 0; --depth) {
            renderer_->pop_clip();
        }
        Outcome outcome;
        outcome.fallbacks = renderer_->clip_shape_fallbacks();
        renderer_->end();
        commands->submit();
        device_->wait_idle();
        const std::vector<std::uint8_t> gpu_pixels = read_target();

        rendering::software::CpuCanvas canvas(64, 64);
        canvas.clear(rhi::rgba8(0, 0, 0));
        const rendering::software::ShapedClip shaped{records, pool};
        canvas.fill_rect_clipped(kBox, kRed, shaped);

        for (std::uint32_t y = 0; y < kExtent.height; ++y) {
            for (std::uint32_t x = 0; x < kExtent.width; ++x) {
                const std::size_t index = (static_cast<std::size_t>(y) * kExtent.width + x) * 4;
                const bool gpu_red = gpu_pixels[index] == 255;
                const bool cpu_red = canvas.pixels()[index] == 255;
                outcome.painted += cpu_red ? 1U : 0U;
                if (gpu_red != cpu_red) {
                    ++outcome.mismatches;
                }
            }
        }
        return outcome;
    }

private:
    Fixture() = default;

    std::vector<std::uint8_t> read_target() {
        const std::size_t bytes =
            static_cast<std::size_t>(kExtent.width) * kExtent.height * sizeof(std::uint32_t);
        auto readback = device_->create_buffer({bytes, rhi::BufferUsage::Readback});
        auto commands = device_->create_command_list();
        commands->copy_target_to_buffer(*target_, *readback);
        commands->submit();
        std::vector<std::uint8_t> pixels(bytes);
        readback->read(pixels.data(), pixels.size());
        return pixels;
    }

    std::unique_ptr<rhi::Device> device_;
    rhi::RenderTargetDesc desc_;
    std::unique_ptr<rhi::RenderTarget> target_;
    std::unique_ptr<rhi::Pipeline> pipeline_;
    std::unique_ptr<rhi::Texture> white_;
    std::unique_ptr<rendering::gpu::Batched2DRenderer> renderer_;
};

} // namespace tessera::test::clipshape
