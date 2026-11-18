// SPDX-License-Identifier: MIT
//
// The renderer's pipeline axis for LCD text (#471, display-aware-rendering.md section 3.4.1 step 3)
// on the null backend, deterministic and GPU-free:
//  - an LCD glyph quad is its own batch identity: it never merges with a grayscale quad over the
//    same texture, and the replay binds the LCD pipeline only around it;
//  - quads of one run over one page merge into a single LCD draw;
//  - the frame pipeline is restored after the replay, so a frame that ends on an LCD batch leaves
//    the pass state the way the next consumer expects;
//  - without an LCD pipeline, `push_texture_lcd` is exactly `push_texture`: no extra bind, no
//  split;
//  - the damage-region replay binds per batch like the plain replay.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/rendering/redraw_plan.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using namespace tessera::rendering;

constexpr std::uint32_t kSize = 64;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "lcd_pipeline_axis: " << message << "\n";
    }
    return condition;
}

rhi::RenderTargetDesc target_desc() {
    rhi::RenderTargetDesc desc;
    desc.extent = {kSize, kSize};
    desc.clear_color = rhi::rgba8(0, 0, 0);
    return desc;
}

struct Fixture {
    std::unique_ptr<rhi::Device> device = null::create_null_device(rhi::DeviceDesc{});
    std::unique_ptr<rhi::RenderTarget> target = device->create_render_target(target_desc());
    std::unique_ptr<rhi::Texture> white = device->create_texture(rhi::TextureDesc{{1, 1}});
    std::unique_ptr<rhi::Texture> page = device->create_texture(rhi::TextureDesc{{8, 8}});
    std::unique_ptr<rhi::Pipeline> pipeline = device->create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::Pipeline> lcd = device->create_pipeline(lcd_desc());

    static rhi::PipelineDesc lcd_desc() {
        rhi::PipelineDesc desc;
        desc.blend = rhi::BlendMode::DualSource;
        return desc;
    }
};

const null::NullCommandStats& stats_of(std::unique_ptr<rhi::CommandList>& commands) {
    return static_cast<null::NullCommandList*>(commands.get())->stats();
}

constexpr rhi::Rect kUv{0, 0, 1, 1};
const rhi::Color kInk = rhi::rgba8(255, 255, 255);

bool lcd_quads_are_their_own_batch() {
    Fixture fixture;
    gpu::Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white);
    renderer.set_lcd_pipeline(fixture.lcd.get());
    auto commands = fixture.device->create_command_list();
    renderer.begin(*commands, *fixture.target, target_desc());
    renderer.push_rect({0, 0, 16, 16}, rhi::rgba8(255, 0, 0));
    renderer.push_texture_lcd({0, 20, 8, 8}, *fixture.page, kUv, kInk);
    renderer.push_texture_lcd({8, 20, 8, 8}, *fixture.page, kUv, kInk);
    renderer.push_texture({0, 30, 8, 8}, *fixture.page, kUv, kInk);
    renderer.push_rect({20, 0, 16, 16}, rhi::rgba8(0, 255, 0));
    renderer.end();
    commands->submit();

    const null::NullCommandStats& stats = stats_of(commands);
    // Rect (frame pipeline), one merged LCD draw, the grayscale glyph over the same page (a split
    // on pipeline alone), then the rect: four draws. Binds: frame, LCD, frame.
    return expect(renderer.lcd_supported(), "the renderer reports the LCD pipeline") &&
           expect(stats.draws == 4, "rect, merged LCD run, grayscale glyph, rect are four draws") &&
           expect(stats.pipeline_binds == 3,
                  "the frame, LCD and frame pipelines are bound in turn");
}

bool the_frame_pipeline_is_restored_after_a_trailing_lcd_batch() {
    Fixture fixture;
    gpu::Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white);
    renderer.set_lcd_pipeline(fixture.lcd.get());
    auto commands = fixture.device->create_command_list();
    renderer.begin(*commands, *fixture.target, target_desc());
    renderer.push_texture_lcd({0, 0, 8, 8}, *fixture.page, kUv, kInk);
    renderer.end();
    commands->submit();

    const null::NullCommandStats& stats = stats_of(commands);
    const auto& order = stats.command_order;
    // begin: frame bind. flush: LCD bind, draw, then the frame pipeline restored before the pass
    // ends, so nothing after the replay inherits the LCD pipeline.
    const auto last_bind =
        std::find(order.rbegin(), order.rend(), null::NullCommandKind::BindPipeline);
    const auto last_draw = std::find(order.rbegin(), order.rend(), null::NullCommandKind::Draw);
    return expect(stats.pipeline_binds == 3, "frame, LCD, then the frame pipeline again") &&
           expect(last_bind != order.rend() && last_draw != order.rend() && last_bind < last_draw,
                  "the final bind comes after the last draw: it is the restore");
}

bool without_a_pipeline_the_lcd_push_is_the_plain_push() {
    Fixture fixture;
    gpu::Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white);
    auto commands = fixture.device->create_command_list();
    renderer.begin(*commands, *fixture.target, target_desc());
    renderer.push_rect({0, 0, 16, 16}, rhi::rgba8(255, 0, 0));
    renderer.push_texture_lcd({0, 20, 8, 8}, *fixture.page, kUv, kInk);
    renderer.push_texture({8, 20, 8, 8}, *fixture.page, kUv, kInk);
    renderer.end();
    commands->submit();

    const null::NullCommandStats& stats = stats_of(commands);
    return expect(!renderer.lcd_supported(), "no pipeline means no LCD support") &&
           expect(stats.draws == 2, "the fallback merges with the grayscale glyph over the page") &&
           expect(stats.pipeline_binds == 1, "only the frame pipeline is ever bound");
}

bool a_damage_replay_binds_per_batch() {
    Fixture fixture;
    gpu::Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white);
    renderer.set_lcd_pipeline(fixture.lcd.get());
    auto commands = fixture.device->create_command_list();
    RedrawPlan plan;
    plan.scissors = {{0, 0, 32, 32}, {32, 32, 32, 32}};
    renderer.begin_damaged(*commands, *fixture.target, target_desc(), plan);
    renderer.push_texture_lcd({4, 4, 8, 8}, *fixture.page, kUv, kInk);
    renderer.push_texture_lcd({36, 36, 8, 8}, *fixture.page, kUv, kInk);
    renderer.end();
    commands->submit();

    const null::NullCommandStats& stats = stats_of(commands);
    // One LCD batch replayed once per region it touches. The pipeline is bound once for the batch
    // (state is bound on the first intersecting region) and restored once at the end.
    return expect(stats.draws >= 1, "the damage replay draws the LCD batch") &&
           expect(stats.pipeline_binds == 3, "frame, LCD, restore - not one bind per region");
}

} // namespace

int main() {
    bool ok = true;
    ok &= lcd_quads_are_their_own_batch();
    ok &= the_frame_pipeline_is_restored_after_a_trailing_lcd_batch();
    ok &= without_a_pipeline_the_lcd_push_is_the_plain_push();
    ok &= a_damage_replay_binds_per_batch();
    return ok ? 0 : 1;
}
