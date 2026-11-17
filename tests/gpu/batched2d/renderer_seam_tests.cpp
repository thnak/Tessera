// SPDX-License-Identifier: MIT
//
// The isolated-layer seam (#242) on the null backend, deterministic and GPU-free:
//  - a break/copy/resume frame records two render passes, one copy_target_region and the same draw
//    count a single-pass frame records, with the requested region and offset inspectable;
//  - a frame that never breaks a pass records exactly today's single-pass command stream;
//  - copy_target_region rejects the misuse a validating backend rejects;
//  - the pooled offscreen target is reused for 300 frames without growing past the
//    glassMaxBlurTargets cap, and respects frames in flight;
//  - a 4x frame is presented through its resolved image (S8): the sampled bind is recorded outside
//    a pass, between the present pipeline bind and the single-sample present pass.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using namespace tessera::rendering;

constexpr std::uint32_t kSize = 64;
constexpr std::uint32_t kQuadVertices = 6;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "renderer_seam: " << message << "\n";
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
    std::unique_ptr<rhi::Pipeline> pipeline = device->create_pipeline(rhi::PipelineDesc{});
};

const null::NullCommandStats& stats_of(std::unique_ptr<rhi::CommandList>& commands) {
    return static_cast<null::NullCommandList*>(commands.get())->stats();
}

bool same_scissor(const rhi::Scissor& a, const rhi::Scissor& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

bool check_break_copy_resume() {
    Fixture fixture;
    gpu::Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white);
    auto commands = fixture.device->create_command_list();
    const rhi::RenderTargetDesc desc = target_desc();
    renderer.begin(*commands, *fixture.target, desc);
    renderer.push_rect({0, 0, 32, 32}, rhi::rgba8(255, 0, 0));
    renderer.flush();

    renderer.break_pass();
    if (!expect(renderer.pass_broken(), "break_pass reports the pass broken")) {
        return false;
    }
    auto* copy_target = renderer.acquire_offscreen_target({40, 40});
    if (!expect(copy_target != nullptr, "the pool hands out an offscreen target")) {
        return false;
    }
    const rhi::Scissor region{8, 8, 32, 32};
    commands->copy_target_region(*fixture.target, region, *copy_target, rhi::Offset2D{4, 4});
    renderer.resume_pass();
    if (!expect(!renderer.pass_broken(), "resume_pass reopens the pass")) {
        return false;
    }
    renderer.push_rect({32, 0, 32, 32}, rhi::rgba8(0, 255, 0));
    renderer.end();
    commands->submit();

    const null::NullCommandStats& stats = stats_of(commands);
    bool ok =
        expect(stats.begin_render_passes == 2, "the break records a second begin_render_pass") &&
        expect(stats.end_render_passes == 2, "the break records the matching end_render_pass") &&
        expect(stats.draws == 2, "draws before and after the break are both recorded") &&
        expect(stats.vertices == 2 * kQuadVertices, "both quads reach the command list") &&
        expect(stats.region_copies == 1, "one copy_target_region is recorded") &&
        expect(stats.target_region_copies.size() == 1, "the copy has one inspectable record") &&
        expect(renderer.offscreen_target_count() == 1, "one pooled target was needed") &&
        expect(renderer.draw_calls() == 2, "the renderer counts both draws");
    if (!ok) {
        return false;
    }
    const null::NullTargetRegionCopy& copy = stats.target_region_copies.front();
    return expect(same_scissor(copy.source_region, region), "the copy records the source region") &&
           expect(copy.destination_offset.x == 4 && copy.destination_offset.y == 4,
                  "the copy records the destination offset") &&
           expect(copy.source_extent.width == kSize && copy.destination_extent.width == 40,
                  "the copy records both target extents");
}

bool check_unbroken_frame_is_unchanged() {
    Fixture fixture;
    gpu::Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white);
    auto commands = fixture.device->create_command_list();
    renderer.begin(*commands, *fixture.target, target_desc());
    renderer.push_rect({0, 0, 16, 16}, rhi::rgba8(255, 0, 0));
    renderer.push_rect({16, 0, 16, 16}, rhi::rgba8(0, 255, 0));
    renderer.end();
    commands->submit();

    const null::NullCommandStats& stats = stats_of(commands);
    return expect(stats.begin_render_passes == 1, "an unbroken frame records one pass") &&
           expect(stats.end_render_passes == 1, "an unbroken frame ends one pass") &&
           expect(stats.region_copies == 0, "an unbroken frame records no copy") &&
           expect(stats.draws == 1 && stats.vertices == 2 * kQuadVertices,
                  "an unbroken frame keeps the single-batch draw") &&
           expect(renderer.offscreen_target_count() == 0,
                  "an unbroken frame allocates no offscreen target");
}

bool check_invalid_copies_are_rejected() {
    Fixture fixture;
    auto destination = fixture.device->create_render_target(rhi::RenderTargetDesc{{32, 32}});
    auto msaa_target = fixture.device->create_render_target(
        rhi::RenderTargetDesc{{32, 32},
                              rhi::TextureFormat::R8G8B8A8Unorm,
                              rhi::Color{0, 0, 0, 1},
                              rhi::LoadOp::Clear,
                              rhi::StoreOp::Store,
                              4});
    auto srgb_target = fixture.device->create_render_target(
        rhi::RenderTargetDesc{{32, 32}, rhi::TextureFormat::R8G8B8A8Srgb});

    const auto throws_invalid = [](auto&& call) {
        try {
            call();
        } catch (const std::invalid_argument&) {
            return true;
        } catch (...) {
            return false;
        }
        return false;
    };
    const auto throws_logic = [](auto&& call) {
        try {
            call();
        } catch (const std::logic_error&) {
            return true;
        } catch (...) {
            return false;
        }
        return false;
    };

    // A copy inside a render pass is a command-order error.
    auto in_pass_commands = fixture.device->create_command_list();
    in_pass_commands->begin_render_pass(*fixture.target, target_desc());
    const bool rejected_in_pass = throws_logic([&] {
        in_pass_commands->copy_target_region(*fixture.target, {0, 0, 8, 8}, *destination, {});
    });
    in_pass_commands->end_render_pass();

    auto commands = fixture.device->create_command_list();
    return expect(rejected_in_pass, "a copy inside a render pass is rejected") &&
           expect(throws_invalid([&] {
                      commands->copy_target_region(*fixture.target, {60, 60, 8, 8}, *destination,
                                                   {});
                  }),
                  "a source region outside the target is rejected") &&
           expect(throws_invalid([&] {
                      commands->copy_target_region(*fixture.target, {0, 0, 32, 32}, *destination,
                                                   {1, 1});
                  }),
                  "a destination offset that overflows is rejected") &&
           expect(throws_invalid([&] {
                      commands->copy_target_region(*msaa_target, {0, 0, 8, 8}, *destination, {});
                  }),
                  "a multisampled source is rejected") &&
           expect(throws_invalid([&] {
                      commands->copy_target_region(*fixture.target, {0, 0, 8, 8}, *srgb_target, {});
                  }),
                  "a format mismatch is rejected") &&
           expect(throws_invalid([&] {
                      commands->copy_target_region(*fixture.target, {0, 0, 8, 8}, *fixture.target,
                                                   {8, 8});
                  }),
                  "a same-target copy is rejected") &&
           expect(throws_invalid([&] {
                      commands->copy_target_region(*fixture.target, {0, 0, 0, 8}, *destination, {});
                  }),
                  "an empty source region is rejected");
}

// Runs `frames` complete frames, each breaking the pass once and copying one region into a pooled
// target, and returns the largest target count observed.
std::uint32_t run_pool_frames(gpu::Batched2DRenderer& renderer, Fixture& fixture,
                              std::uint32_t frames) {
    std::uint32_t largest = 0;
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        auto commands = fixture.device->create_command_list();
        renderer.begin(*commands, *fixture.target, target_desc());
        renderer.push_rect({0, 0, 32, 32}, rhi::rgba8(255, 0, 0));
        renderer.break_pass();
        auto* copy_target = renderer.acquire_offscreen_target({32, 32});
        if (copy_target == nullptr) {
            return 0;
        }
        commands->copy_target_region(*fixture.target, {0, 0, 32, 32}, *copy_target, {});
        renderer.resume_pass();
        renderer.push_rect({32, 0, 32, 32}, rhi::rgba8(0, 255, 0));
        renderer.end();
        commands->submit();
        largest = std::max(largest, renderer.offscreen_target_count());
    }
    return largest;
}

bool check_pool_is_bounded_over_300_frames() {
    Fixture fixture;
    gpu::Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white);
    if (!expect(renderer.offscreen_target_count() == 0,
                "the pool starts empty before the first frame")) {
        return false;
    }
    const std::uint32_t largest = run_pool_frames(renderer, fixture, 300);
    return expect(largest == 1, "a steady-state frame reuses one target for 300 frames") &&
           expect(renderer.offscreen_target_count() == 1,
                  "the target count does not grow with frames") &&
           expect(renderer.offscreen_target_count() <= 4,
                  "the target count stays under glassMaxBlurTargets");
}

bool check_pool_respects_frames_in_flight() {
    Fixture fixture;
    gpu::Batched2DConfig config;
    config.frames_in_flight = 2;
    gpu::Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, config);
    const std::uint32_t largest = run_pool_frames(renderer, fixture, 300);
    return expect(largest == 2, "two frames in flight need exactly two targets") &&
           expect(renderer.offscreen_target_count() == 2, "the pool settles at frames_in_flight") &&
           expect(renderer.offscreen_target_count() <= 4, "the target count stays capped");
}

bool check_pool_returns_null_at_the_cap() {
    Fixture fixture;
    gpu::Batched2DConfig config;
    config.frames_in_flight = 4;
    config.max_offscreen_targets = 1;
    gpu::Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white, config);

    const auto frame = [&] {
        auto commands = fixture.device->create_command_list();
        renderer.begin(*commands, *fixture.target, target_desc());
        auto* copy_target = renderer.acquire_offscreen_target({16, 16});
        renderer.end();
        commands->submit();
        return copy_target;
    };
    rhi::RenderTarget* first = frame();
    rhi::RenderTarget* blocked = frame(); // frame 1 is still in flight (window of 4)
    const std::uint32_t count_while_blocked = renderer.offscreen_target_count();
    // Advance past the in-flight window: the target becomes recyclable again.
    rhi::RenderTarget* recycled = nullptr;
    for (int skipped = 0; skipped < 5 && recycled == nullptr; ++skipped) {
        recycled = frame();
    }
    return expect(first != nullptr, "the first acquire gets a target") &&
           expect(blocked == nullptr, "acquire returns null while the only target is in flight") &&
           expect(count_while_blocked == 1, "a blocked acquire does not grow the pool") &&
           expect(recycled != nullptr, "the target is recycled once its frame completed") &&
           expect(renderer.offscreen_target_count() == 1, "the pool never exceeds its cap");
}

bool check_pool_grows_a_free_target() {
    Fixture fixture;
    gpu::Batched2DRenderer renderer(*fixture.device, *fixture.pipeline, *fixture.white);
    auto commands = fixture.device->create_command_list();
    renderer.begin(*commands, *fixture.target, target_desc());
    rhi::RenderTarget* small = renderer.acquire_offscreen_target({16, 16});
    renderer.end();
    commands->submit();

    auto larger_commands = fixture.device->create_command_list();
    renderer.begin(*larger_commands, *fixture.target, target_desc());
    rhi::RenderTarget* large = renderer.acquire_offscreen_target({48, 48});
    renderer.end();
    larger_commands->submit();

    return expect(small != nullptr && large != nullptr, "both acquires get a target") &&
           expect(renderer.offscreen_target_count() == 1,
                  "growing reuses the free slot instead of adding a target") &&
           expect(large->extent().width >= 48 && large->extent().height >= 48,
                  "the recycled target grew to the requested extent");
}

// A 4x frame is presented through its resolved image with one sampled-quad pass (S8): the
// multisample image is not a copy/blit source, so this is the only GPU-only present path. On the
// null backend the recorded order is the contract: scene pass, sampled bind, then a single-sample
// present pass.
bool check_present_target_through_the_resolved_image() {
    Fixture fixture;
    const rhi::RenderTargetDesc scene_desc = [] {
        rhi::RenderTargetDesc desc = target_desc();
        desc.sample_count = 4;
        return desc;
    }();
    auto scene_target = fixture.device->create_render_target(scene_desc);
    auto destination = fixture.device->create_render_target(target_desc());
    auto pipeline4 = fixture.device->create_pipeline([] {
        rhi::PipelineDesc desc;
        desc.sample_count = 4;
        return desc;
    }());
    auto present_pipeline = fixture.device->create_pipeline([] {
        rhi::PipelineDesc desc;
        desc.sample_count = 1;
        desc.blend = rhi::BlendMode::None;
        return desc;
    }());

    gpu::Batched2DConfig config;
    config.sample_count = 4;
    gpu::Batched2DRenderer renderer(*fixture.device, *pipeline4, *fixture.white, config);
    auto commands = fixture.device->create_command_list();
    renderer.begin(*commands, *scene_target, scene_desc);
    renderer.push_rect({0, 0, 32, 32}, rhi::rgba8(255, 0, 0));
    renderer.end();

    // The present call is only legal after end(): the source cannot be sampled inside its own pass.
    // Checked on its own renderer so the main frame's command list keeps one frame's vertex data.
    bool rejected_in_frame = false;
    {
        gpu::Batched2DRenderer in_frame_renderer(*fixture.device, *pipeline4, *fixture.white,
                                                 config);
        auto in_frame_commands = fixture.device->create_command_list();
        in_frame_renderer.begin(*in_frame_commands, *scene_target, scene_desc);
        try {
            in_frame_renderer.present_target(*in_frame_commands, *destination, target_desc(),
                                             *scene_target, *present_pipeline);
        } catch (const std::logic_error&) {
            rejected_in_frame = true;
        }
        in_frame_renderer.end();
    }

    renderer.present_target(*commands, *destination, target_desc(), *scene_target,
                            *present_pipeline);
    commands->submit();

    const null::NullCommandStats& stats = stats_of(commands);
    bool ok = expect(rejected_in_frame, "present_target inside a frame is rejected") &&
              expect(stats.begin_render_passes == 2 && stats.end_render_passes == 2,
                     "the present records its own pass after the scene pass") &&
              expect(stats.sampled_targets.size() == 1, "one sampled target is bound") &&
              expect(stats.sampled_targets.front().sample_count == 4,
                     "the 4x target is sampled through its resolved image") &&
              expect(stats.last_render_pass_sample_count == 1,
                     "the present pass renders into the 1-sample destination") &&
              expect(stats.draws == 2 && stats.vertices == 2 * kQuadVertices,
                     "the presentation is one full-target quad") &&
              expect(renderer.draw_calls() == 2, "the renderer counts the present draw");
    if (!ok) {
        return false;
    }
    // The sampled bind is recorded outside a pass, after the pipeline bind and before the present
    // pass opens: bind_pipeline -> bind_sampled_target -> begin_render_pass.
    const std::vector<null::NullCommandKind>& order = stats.command_order;
    const auto sampled =
        std::find(order.begin(), order.end(), null::NullCommandKind::BindSampledTarget);
    if (!expect(sampled != order.end(), "the sampled bind is recorded") ||
        !expect(sampled != order.begin() && sampled + 1 != order.end(),
                "the sampled bind has a recorded neighbour")) {
        return false;
    }
    return expect(*(sampled - 1) == null::NullCommandKind::BindPipeline,
                  "the present pipeline is bound before the sampled target") &&
           expect(*(sampled + 1) == null::NullCommandKind::BeginRenderPass,
                  "the present pass opens after the sampled bind");
}

} // namespace

int main() {
    if (!check_break_copy_resume()) {
        return 1;
    }
    if (!check_unbroken_frame_is_unchanged()) {
        return 2;
    }
    if (!check_invalid_copies_are_rejected()) {
        return 3;
    }
    if (!check_pool_is_bounded_over_300_frames()) {
        return 4;
    }
    if (!check_pool_respects_frames_in_flight()) {
        return 5;
    }
    if (!check_pool_returns_null_at_the_cap()) {
        return 6;
    }
    if (!check_pool_grows_a_free_target()) {
        return 7;
    }
    if (!check_present_target_through_the_resolved_image()) {
        return 8;
    }
    std::cout << "renderer_seam: ok\n";
    return 0;
}
