// SPDX-License-Identifier: MIT
//
// Real-Vulkan `clip-path: polygon()` readback (ADR-068 slice 5, issue #673). Each scene paints one
// full-target quad under a shaped clip and compares every pixel with the CPU reference
// (`CpuCanvas::fill_rect_clipped`, which uses `gpu::clip::covers_pixel`). The fixtures keep every
// edge crossing at least a thousandth of a pixel from a pixel centre, so the GPU and the CPU can
// only disagree when the shader's coverage rule is wrong, not when they round differently.
//
// Deterministic: UNORM target, AA disabled (golden config), exact byte comparison.

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
#include <string>
#include <vector>

namespace {

namespace clip = tessera::rendering::gpu::clip;
namespace detail = tessera::ui::paint::detail;
namespace rhi = tessera::rendering::rhi;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::BufferUsage;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::LoadOp;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::StoreOp;
using tessera::rendering::rhi::TextureDesc;

constexpr Extent2D kExtent{64, 64};
constexpr Color kBackground{0.0f, 0.0f, 0.0f, 1.0f};
constexpr Color kRed = rgba8(255, 0, 0);
constexpr rhi::Rect kBox{0.0f, 0.0f, 64.0f, 64.0f};

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "polygon_clip: " << message << "\n";
    }
    return condition;
}

RenderTargetDesc target_desc() {
    RenderTargetDesc desc;
    desc.extent = kExtent;
    desc.clear_color = kBackground;
    desc.load_op = LoadOp::Clear;
    desc.store_op = StoreOp::Store;
    return desc;
}

std::vector<std::uint8_t> read_target(Device& device, RenderTarget& target) {
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

// One scene: the shapes pushed outermost first. A shape is a parsed `clip-path` value, so the test
// goes through the same parser the engine will.
struct Scene {
    std::string name;
    std::vector<std::string> shapes;
    // True when the first shape is pushed as a plain rounded rect instead of a `clip-path` value.
    bool rounded_outer = false;
    std::uint32_t expected_fallbacks = 0;
};

std::string zigzag(std::uint32_t count) {
    std::string css = "polygon(";
    for (std::uint32_t index = 0; index < count; ++index) {
        css += index == 0 ? "" : ", ";
        css += std::to_string(index % 2 == 0 ? 8 : 56) + "px " +
               std::to_string(index % 4 < 2 ? 8 : 56) + "px";
    }
    return css + ")";
}

const char* const kTriangle = "polygon(8.25px 8.25px, 56.25px 8.25px, 32.25px 56.25px)";
const char* const kLShape =
    "polygon(10.25px 10.25px, 40.25px 10.25px, 40.25px 30.25px, 25.25px 30.25px, "
    "25.25px 50.25px, 10.25px 50.25px)";
const char* const kStarNonzero =
    "polygon(nonzero, 32.1px 4.1px, 48.56px 54.75px, 5.47px 23.45px, 58.73px 23.45px, "
    "15.64px 54.75px)";
const char* const kStarEvenOdd =
    "polygon(evenodd, 32.1px 4.1px, 48.56px 54.75px, 5.47px 23.45px, 58.73px 23.45px, "
    "15.64px 54.75px)";

// Runs one scene on the GPU and on the CPU reference and returns the number of pixels that differ.
struct Outcome {
    std::size_t mismatches = 0;
    std::size_t painted = 0;
    std::uint32_t fallbacks = 0;
};

Outcome run_scene(Device& device, Batched2DRenderer& renderer, RenderTarget& target,
                  const RenderTargetDesc& desc, const Scene& scene) {
    std::vector<clip::ClipRecord> records;
    std::vector<clip::ClipVertex> pool;
    std::vector<detail::ClipShape> parsed;
    for (const std::string& css : scene.shapes) {
        parsed.push_back(detail::parse_clip_path(css, kBox).shape);
    }

    auto commands = device.create_command_list();
    renderer.begin(*commands, target, desc);
    std::size_t depth = 0;
    if (scene.rounded_outer) {
        const clip::RoundedRect outer = clip::uniform_rect({8.0f, 8.0f, 48.0f, 48.0f}, 16.0f);
        renderer.push_clip(outer);
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
        renderer.push_clip_shape(shaped);
        ++depth;
        // The CPU reference sees the pool the table would build: this scene's polygons appended in
        // order (no scene repeats a polygon, so there is nothing to deduplicate).
        if (record.kind == static_cast<std::uint32_t>(clip::RecordKind::Polygon)) {
            if (shape.vertices.size() > clip::kMaxPolygonVertices) {
                // Over the limit the table degrades to the bounding box: the same rect, no polygon.
                record.kind = static_cast<std::uint32_t>(clip::RecordKind::RoundedRect);
                record.vertex_count = 0;
            } else {
                record.first_vertex = static_cast<std::uint32_t>(pool.size());
                pool.insert(pool.end(), shape.vertices.begin(), shape.vertices.end());
            }
        }
        records.push_back(record);
    }
    renderer.push_rect({0, 0, 64, 64}, kRed);
    for (; depth > 0; --depth) {
        renderer.pop_clip();
    }
    Outcome outcome;
    outcome.fallbacks = renderer.clip_shape_fallbacks();
    renderer.end();
    commands->submit();
    device.wait_idle();
    const std::vector<std::uint8_t> gpu_pixels = read_target(device, target);

    tessera::rendering::software::CpuCanvas canvas(64, 64);
    canvas.clear(rgba8(0, 0, 0));
    const tessera::rendering::software::ShapedClip shaped{records, pool};
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

} // namespace

int main() {
    using tessera::rendering::rhi::Backend;
    using tessera::rendering::rhi::DeviceDesc;

    DeviceDesc device_desc;
    device_desc.backend = Backend::Vulkan;
    device_desc.validation = true;

    std::unique_ptr<Device> device;
    try {
        device = tessera::rendering::vulkan::create_vulkan_device(device_desc);
    } catch (const std::exception& error) {
        std::cout << "polygon_clip: skipped (Vulkan device unavailable: " << error.what() << ")\n";
        return 0;
    }

    const RenderTargetDesc desc = target_desc();
    auto target = device->create_render_target(desc);
    auto pipeline = device->create_pipeline(tessera::rendering::vulkan::batched2d_pipeline_desc());

    const std::array<std::uint8_t, 4> white_pixel = {255, 255, 255, 255};
    TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.pixels = white_pixel.data();
    white_desc.row_pitch = 4;
    auto white = device->create_texture(white_desc);

    // AA off + golden mode: hard edges and exact bytes.
    Batched2DRenderer renderer(*device, *pipeline, *white, Batched2DConfig{4096, false, true});

    const std::vector<Scene> scenes = {
        {"convex triangle", {kTriangle}, false, 0},
        {"concave L", {kLShape}, false, 0},
        {"self-intersecting star, nonzero", {kStarNonzero}, false, 0},
        {"self-intersecting star, evenodd", {kStarEvenOdd}, false, 0},
        {"polygon under a rounded clip", {kStarNonzero}, true, 0},
        {"over the vertex limit", {zigzag(clip::kMaxPolygonVertices + 1)}, false, 1},
    };

    bool ok = true;
    std::vector<std::size_t> painted;
    for (const Scene& scene : scenes) {
        const Outcome outcome = run_scene(*device, renderer, *target, desc, scene);
        ok =
            expect(outcome.mismatches == 0, scene.name + ": " + std::to_string(outcome.mismatches) +
                                                " pixel(s) differ from the CPU reference") &&
            ok;
        ok = expect(outcome.painted > 0, scene.name + ": the reference paints something") && ok;
        ok = expect(outcome.fallbacks == scene.expected_fallbacks,
                    scene.name + ": the fallback count is " +
                        std::to_string(scene.expected_fallbacks)) &&
             ok;
        painted.push_back(outcome.painted);
    }
    // The two fill rules must differ on the star, or the test would pass with one rule ignored.
    ok = expect(painted[2] > painted[3], "nonzero paints more of the star than evenodd") && ok;
    // The over-limit polygon is its bounding box: 48 x 48 pixels.
    ok = expect(painted[5] == 48U * 48U, "an over-limit polygon paints its bounding box") && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "polygon_clip: ok\n";
    return 0;
}
