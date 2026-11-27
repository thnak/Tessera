// SPDX-License-Identifier: MIT
//
// Per-fragment gradients on the null backend (docs/design/gpu/gradients.md, ADR-076), driven
// through Batched2DSink and Batched2DRenderer so the recorded geometry is what is asserted:
//  - one gradient layer is one quad (6 vertices, 384 bytes) where the banded path emitted 64 solid
//    quads for a linear gradient and a 32 x 32 / 64 x 64 grid for a radial / conic one - the counts
//    are printed so the saving is a measured number, not a claim;
//  - the banded fallback is exactly what the engine drew before, selected by the capability: a
//  frame
//    pipeline without the record buffer (a fixture pipeline) never takes the shader path;
//  - a culled gradient takes no record and splits no batch, a full table falls back per draw and is
//    counted, a rounded clip is pushed and popped around the one quad;
//  - one hundred gradients are one batch.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>

namespace {

namespace rhi = tessera::rendering::rhi;
namespace gradient = tessera::rendering::gpu::gradient;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::kVertex2DStride;
using tessera::ui::LinearGradientStop;
using tessera::ui::paint::Batched2DSink;
using tessera::ui::paint::ConicGradientDraw;
using tessera::ui::paint::GradientDraw;
using tessera::ui::paint::RadialGradientDraw;

constexpr std::uint32_t kSize = 256;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "gradient_null: " << message << "\n";
    }
    return condition;
}

// A null device, a frame pipeline that does or does not declare the record buffer, and a renderer.
struct Fixture {
    explicit Fixture(bool record_buffer, Batched2DConfig config = {})
        : device(tessera::rendering::null::create_null_device({})),
          target(device->create_render_target(rhi::RenderTargetDesc{{kSize, kSize}})),
          white(device->create_texture(rhi::TextureDesc{{1, 1}})),
          pipeline(make_pipeline(record_buffer)),
          commands(device->create_command_list()),
          renderer(*device, *pipeline, *white, config) {
        rhi::RenderTargetDesc desc;
        desc.extent = {kSize, kSize};
        renderer.begin(*commands, *target, desc);
    }

    std::unique_ptr<rhi::Pipeline> make_pipeline(bool record_buffer) {
        rhi::PipelineDesc desc;
        desc.storage_buffer = record_buffer;
        return device->create_pipeline(desc);
    }

    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<rhi::RenderTarget> target;
    std::unique_ptr<rhi::Texture> white;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::CommandList> commands;
    Batched2DRenderer renderer;
};

const rhi::Rect kBox{20.0f, 20.0f, 200.0f, 160.0f};

template<typename Draw> void set_stops(Draw& draw) {
    draw.stops[0] = LinearGradientStop{0.0f, rhi::Color{1.0f, 0.0f, 0.0f, 1.0f}};
    draw.stops[1] = LinearGradientStop{1.0f, rhi::Color{0.0f, 0.0f, 1.0f, 1.0f}};
    draw.stop_count = 2;
}

GradientDraw linear_draw() {
    GradientDraw draw;
    draw.rect = kBox;
    draw.angle_degrees = 60.0f;
    set_stops(draw);
    return draw;
}

RadialGradientDraw radial_draw() {
    RadialGradientDraw draw;
    draw.rect = kBox;
    draw.center_x = 120.0f;
    draw.center_y = 100.0f;
    draw.radius_x = 100.0f;
    draw.radius_y = 80.0f;
    set_stops(draw);
    return draw;
}

ConicGradientDraw conic_draw() {
    ConicGradientDraw draw;
    draw.rect = kBox;
    draw.center_x = 120.0f;
    draw.center_y = 100.0f;
    set_stops(draw);
    return draw;
}

struct Counts {
    std::uint32_t vertices = 0;
    std::uint32_t quads = 0;
    std::uint32_t fallbacks = 0;
};

template<typename Draw, typename Fill>
Counts paint_one(bool record_buffer, const Draw& draw, Fill fill) {
    Fixture fixture(record_buffer);
    Batched2DSink sink(fixture.renderer);
    (sink.*fill)(draw);
    Counts counts;
    counts.vertices = fixture.renderer.vertex_count();
    counts.quads = fixture.renderer.gradient_quads();
    counts.fallbacks = fixture.renderer.gradient_fallbacks();
    fixture.renderer.end();
    fixture.commands->submit();
    return counts;
}

void report(const char* name, const Counts& before, const Counts& after) {
    std::cout << "gradient_null: " << name << ": banded " << before.vertices / 6 << " quads, "
              << before.vertices * kVertex2DStride << " vertex bytes; shader " << after.vertices / 6
              << " quad, " << after.vertices * kVertex2DStride << " vertex bytes\n";
}

bool one_quad_per_layer() {
    bool ok = true;
    const Counts linear_before =
        paint_one(false, linear_draw(), &Batched2DSink::fill_linear_gradient);
    const Counts linear_after =
        paint_one(true, linear_draw(), &Batched2DSink::fill_linear_gradient);
    const Counts radial_before =
        paint_one(false, radial_draw(), &Batched2DSink::fill_radial_gradient);
    const Counts radial_after =
        paint_one(true, radial_draw(), &Batched2DSink::fill_radial_gradient);
    const Counts conic_before = paint_one(false, conic_draw(), &Batched2DSink::fill_conic_gradient);
    const Counts conic_after = paint_one(true, conic_draw(), &Batched2DSink::fill_conic_gradient);
    report("linear", linear_before, linear_after);
    report("radial", radial_before, radial_after);
    report("conic", conic_before, conic_after);

    ok =
        expect(linear_after.vertices == 6 && linear_after.quads == 1 && linear_after.fallbacks == 0,
               "a linear gradient is one quad") &&
        ok;
    ok = expect(radial_after.vertices == 6 && radial_after.quads == 1,
                "a radial gradient is one quad") &&
         ok;
    ok = expect(conic_after.vertices == 6 && conic_after.quads == 1,
                "a conic gradient is one quad") &&
         ok;
    // The fallback is the old emission: 64 bands, the cells inside the ellipse of a 32 x 32 grid,
    // the 64 x 64 grid of a conic.
    ok = expect(linear_before.vertices == 64 * 6 && linear_before.quads == 0 &&
                    linear_before.fallbacks == 1,
                "the banded fallback is 64 bands and is counted") &&
         ok;
    ok = expect(radial_before.vertices / 6 > 500 && radial_before.vertices / 6 <= 32 * 32,
                "the banded radial is a grid of cells inside the ellipse") &&
         ok;
    ok = expect(conic_before.vertices == 64 * 64 * 6,
                "the banded conic is the whole 64 x 64 grid") &&
         ok;
    return ok;
}

bool rounded_clip_is_balanced() {
    Fixture fixture(true);
    Batched2DSink sink(fixture.renderer);
    GradientDraw draw = linear_draw();
    draw.clip = tessera::ui::paint::RoundedClip{kBox, 12.0f, 12.0f, 12.0f, 12.0f};
    sink.fill_linear_gradient(draw);
    // A pop without a matching push would throw, and an unpopped clip would leave the next quad in
    // a clip set of its own: the rect pushed after the gradient must share the empty set's batch.
    fixture.renderer.push_rect({0.0f, 0.0f, 4.0f, 4.0f}, rhi::Color{1, 1, 1, 1});
    bool ok = expect(fixture.renderer.vertex_count() == 12, "a rounded gradient is still one quad");
    // The push met an empty batch (no split); the pop ended the clipped quad's batch.
    ok = expect(fixture.renderer.batch_split_stats().clip == 1,
                "the rounded clip opened and closed around the one quad") &&
         ok;
    return ok;
}

bool culled_gradient_takes_no_record() {
    Fixture fixture(true);
    fixture.renderer.set_scissor(rhi::Scissor{0, 0, 10, 10});
    gradient::Fill fill;
    const std::array<gradient::Stop, 2> stops{
        {{0.0f, rhi::Color{1, 0, 0, 1}}, {1.0f, rhi::Color{0, 0, 1, 1}}}};
    fill.rect = {100.0f, 100.0f, 50.0f, 50.0f}; // far from the scissor
    fill.stops = stops;
    const bool handled = fixture.renderer.push_gradient(fill);
    bool ok = expect(handled, "a culled gradient is handled, not refused");
    ok = expect(fixture.renderer.vertex_count() == 0 && fixture.renderer.gradient_quads() == 0 &&
                    fixture.renderer.gradient_fallbacks() == 0,
                "a culled gradient emits nothing and is not a fallback") &&
         ok;
    return ok;
}

bool table_limit_and_batching() {
    Fixture fixture(true);
    Batched2DSink sink(fixture.renderer);
    GradientDraw draw = linear_draw();
    draw.rect = {10.0f, 10.0f, 20.0f, 20.0f};
    for (std::uint32_t index = 0; index < gradient::kMaxGradientRecords; ++index) {
        sink.fill_linear_gradient(draw);
    }
    bool ok = expect(fixture.renderer.gradient_quads() == gradient::kMaxGradientRecords &&
                         fixture.renderer.gradient_fallbacks() == 0,
                     "the table takes its capacity");
    // One batch for all of them: nothing about a gradient splits it.
    ok = expect(fixture.renderer.pending_batches() == 1, "512 gradients are one batch") && ok;
    const std::uint32_t before = fixture.renderer.vertex_count();
    sink.fill_linear_gradient(draw);
    ok = expect(fixture.renderer.gradient_fallbacks() == 1 &&
                    fixture.renderer.vertex_count() == before + 64 * 6,
                "the next gradient falls back to bands, counted") &&
         ok;
    return ok;
}

bool frame_reset_restores_capacity() {
    Fixture fixture(true);
    Batched2DSink sink(fixture.renderer);
    GradientDraw draw = linear_draw();
    for (std::uint32_t index = 0; index < gradient::kMaxGradientRecords + 4; ++index) {
        sink.fill_linear_gradient(draw);
    }
    fixture.renderer.end();
    fixture.commands->submit();
    rhi::RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    auto commands = fixture.device->create_command_list();
    fixture.renderer.begin(*commands, *fixture.target, desc);
    sink.fill_linear_gradient(draw);
    bool ok =
        expect(fixture.renderer.gradient_quads() == 1 && fixture.renderer.gradient_fallbacks() == 0,
               "a new frame starts with an empty table and zeroed counters");
    fixture.renderer.end();
    commands->submit();
    return ok;
}

} // namespace

int main() {
    bool ok = one_quad_per_layer();
    ok = rounded_clip_is_balanced() && ok;
    ok = culled_gradient_takes_no_record() && ok;
    ok = table_limit_and_batching() && ok;
    ok = frame_reset_restores_capacity() && ok;
    if (!ok) {
        std::cerr << "gradient_null: failed\n";
        return 1;
    }
    std::cout << "gradient_null: ok\n";
    return 0;
}
