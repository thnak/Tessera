// SPDX-License-Identifier: MIT
//
// RHI 1.3 sampled render target (issue #254) on the null backend, deterministic and GPU-free. The
// capability is what the glass composite draws with, so this pins the recording order a real frame
// uses - attachment -> copy -> sample -> composite - plus the sampler state that travels with the
// target and the misuse every backend rejects.
//
// The null backend samples nothing; `NullCommandStats::command_order` is the record these tests
// assert. The pixel proof is tests/gpu/batched2d/glass_composite_gpu_tests.cpp.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using namespace tessera::rendering;

constexpr std::uint32_t kSize = 64;
constexpr std::uint32_t kQuadVertices = 6;
constexpr std::uint32_t kFullscreenVertices = 3;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "sampled_target: " << message << "\n";
    }
    return condition;
}

rhi::RenderTargetDesc target_desc(std::uint32_t size = kSize) {
    rhi::RenderTargetDesc desc;
    desc.extent = {size, size};
    desc.clear_color = rhi::rgba8(0, 0, 0);
    return desc;
}

rhi::RenderTargetDesc load_target_desc() {
    rhi::RenderTargetDesc desc = target_desc();
    desc.load_op = rhi::LoadOp::Load;
    return desc;
}

struct Fixture {
    std::unique_ptr<rhi::Device> device = null::create_null_device(rhi::DeviceDesc{});
    std::unique_ptr<rhi::RenderTarget> frame = device->create_render_target(target_desc());
    std::unique_ptr<rhi::Texture> white = device->create_texture(rhi::TextureDesc{{1, 1}});
    std::unique_ptr<rhi::Pipeline> batched2d = device->create_pipeline(rhi::PipelineDesc{});
    std::unique_ptr<rhi::Pipeline> glass = device->create_pipeline(rhi::PipelineDesc{});
};

const null::NullCommandStats& stats_of(std::unique_ptr<rhi::CommandList>& commands) {
    return static_cast<null::NullCommandList*>(commands.get())->stats();
}

std::size_t index_of(const null::NullCommandStats& stats, null::NullCommandKind kind) {
    for (std::size_t index = 0; index < stats.command_order.size(); ++index) {
        if (stats.command_order[index] == kind) {
            return index;
        }
    }
    return stats.command_order.size();
}

std::size_t last_index_of(const null::NullCommandStats& stats, null::NullCommandKind kind) {
    for (std::size_t index = stats.command_order.size(); index > 0; --index) {
        if (stats.command_order[index - 1] == kind) {
            return index - 1;
        }
    }
    return stats.command_order.size();
}

// The whole seam in recording order: draw into the frame target (attachment), copy a region into a
// pooled target (copy), sample that copy into a blur target (sample), sample the blur target and
// composite it back into the frame (composite).
bool check_composite_order() {
    Fixture fixture;
    auto backdrop = fixture.device->create_render_target(target_desc());
    auto blur = fixture.device->create_render_target(target_desc());
    auto commands = fixture.device->create_command_list();
    const rhi::RenderTargetDesc desc = target_desc();
    gpu::GlassPushConstants constants;
    constants.texel_step_x = 1.0f / static_cast<float>(kSize);
    constants.texel_step_y = constants.texel_step_x;
    constants.sigma = 2.0f;
    constants.radius = 6.0f;

    commands->begin_render_pass(*fixture.frame, desc);
    commands->bind_pipeline(*fixture.batched2d);
    commands->set_viewport({0.0f, 0.0f, 64.0f, 64.0f});
    commands->set_scissor({0, 0, kSize, kSize});
    commands->draw(kQuadVertices, 0);
    commands->end_render_pass();

    commands->copy_target_region(*fixture.frame, {0, 0, kSize, kSize}, *backdrop, rhi::Offset2D{});

    commands->bind_pipeline(*fixture.glass);
    commands->bind_sampled_target(0, *backdrop);
    commands->begin_render_pass(*blur, desc);
    commands->set_scissor({0, 0, kSize, kSize});
    commands->push_constants(&constants, sizeof(constants));
    commands->draw(kFullscreenVertices);
    commands->end_render_pass();

    commands->bind_sampled_target(0, *blur);
    commands->begin_render_pass(*fixture.frame, load_target_desc());
    commands->bind_pipeline(*fixture.glass);
    constants.mode = 1.0f;
    commands->push_constants(&constants, sizeof(constants));
    commands->draw(kFullscreenVertices);
    commands->end_render_pass();
    commands->submit();

    const null::NullCommandStats& stats = stats_of(commands);
    const std::array<null::NullCommandKind, 16> expected = {
        null::NullCommandKind::BeginRenderPass,
        null::NullCommandKind::BindPipeline,
        null::NullCommandKind::Draw,
        null::NullCommandKind::EndRenderPass,
        null::NullCommandKind::CopyTargetRegion,
        null::NullCommandKind::BindPipeline,
        null::NullCommandKind::BindSampledTarget,
        null::NullCommandKind::BeginRenderPass,
        null::NullCommandKind::Draw,
        null::NullCommandKind::EndRenderPass,
        null::NullCommandKind::BindSampledTarget,
        null::NullCommandKind::BeginRenderPass,
        null::NullCommandKind::BindPipeline,
        null::NullCommandKind::Draw,
        null::NullCommandKind::EndRenderPass,
        null::NullCommandKind::Submit,
    };
    bool ok =
        expect(stats.command_order.size() == expected.size(),
               "the frame records the expected number of commands") &&
        expect(stats.command_order ==
                   std::vector<null::NullCommandKind>(expected.begin(), expected.end()),
               "the command order is attachment -> copy -> sample -> composite") &&
        expect(stats.begin_render_passes == 3 && stats.end_render_passes == 3,
               "three balanced passes: frame, blur target, frame resumed") &&
        expect(stats.region_copies == 1, "one backdrop region copy") &&
        expect(stats.sampled_target_binds == 2, "two sampled targets: copy and blur result") &&
        expect(stats.draws == 3, "two fullscreen blur/composite draws and one rectangle") &&
        expect(stats.sampled_targets.size() == 2, "both binds have an inspectable record");
    if (!ok) {
        return false;
    }
    const null::NullSampledTargetBind& first = stats.sampled_targets.front();
    const null::NullSampledTargetBind& second = stats.sampled_targets.back();
    return expect(first.slot == 0, "the sampled bind records the descriptor slot") &&
           expect(first.extent.width == kSize && first.extent.height == kSize,
                  "the sampled bind records the target extent") &&
           expect(first.format == rhi::TextureFormat::R8G8B8A8Unorm,
                  "the sampled bind records the target format") &&
           expect(first.sample_count == 1, "the sampled bind records the sample count") &&
           expect(first.filter == rhi::Filter::Linear &&
                      first.address == rhi::AddressMode::ClampToEdge,
                  "the default sampled bind is linear and clamp-to-edge") &&
           expect(second.extent.width == kSize, "the second bind is the blur target");
}

// A target's sampler state comes from RenderTargetDesc and reaches the command list unchanged, so
// the blur can rely on bilinear taps and a border node cannot darken.
bool check_sampler_state_from_desc() {
    Fixture fixture;
    rhi::RenderTargetDesc custom = target_desc(32);
    custom.filter = rhi::Filter::Nearest;
    custom.address = rhi::AddressMode::Repeat;
    auto target = fixture.device->create_render_target(custom);

    auto commands = fixture.device->create_command_list();
    commands->begin_render_pass(*target, custom);
    commands->end_render_pass();
    commands->bind_pipeline(*fixture.glass);
    commands->bind_sampled_target(1, *target);
    commands->draw(0);
    commands->submit();

    const null::NullCommandStats& stats = stats_of(commands);
    return expect(stats.sampled_targets.size() == 1, "the bind is recorded") &&
           expect(stats.sampled_targets.front().slot == 1,
                  "the descriptor slot reaches the backend") &&
           expect(stats.sampled_targets.front().filter == rhi::Filter::Nearest,
                  "the target's filter is the descriptor's") &&
           expect(stats.sampled_targets.front().address == rhi::AddressMode::Repeat,
                  "the target's address mode is the descriptor's") &&
           expect(stats.sampled_targets.front().extent.width == 32,
                  "the sampled bind sees the target extent");
}

// The misuse the Vulkan and WebGPU backends reject, rejected on null too (validating-backend
// parity).
bool check_invalid_binds_are_rejected() {
    Fixture fixture;
    auto empty = fixture.device->create_render_target(target_desc());
    auto commands = fixture.device->create_command_list();

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

    // Sampling needs the pass closed: the layout transition belongs outside a pass.
    commands->begin_render_pass(*fixture.frame, target_desc());
    commands->bind_pipeline(*fixture.glass);
    const bool rejected_in_pass =
        throws_logic([&] { commands->bind_sampled_target(0, *fixture.frame); });
    commands->end_render_pass();

    // A target that was never written has no pixels and no defined state to sample.
    const bool rejected_empty = throws_invalid([&] { commands->bind_sampled_target(0, *empty); });

    return expect(rejected_in_pass, "a sampled bind inside a render pass is rejected") &&
           expect(rejected_empty, "sampling a target without rendered content is rejected");
}

// The renderer path: break the pass, copy into a pooled target, run the horizontal pass into a
// second pooled target, then resume the frame and composite. The pool's retirement is the #242
// contract; this test pins that the sampled seam composes with it in one frame.
bool check_renderer_pooled_composite() {
    Fixture fixture;
    gpu::Batched2DConfig config;
    gpu::Batched2DRenderer renderer(*fixture.device, *fixture.batched2d, *fixture.white, config);
    auto commands = fixture.device->create_command_list();
    const rhi::RenderTargetDesc desc = target_desc();

    renderer.begin(*commands, *fixture.frame, desc);
    renderer.push_rect({0, 0, 32, 32}, rhi::rgba8(255, 0, 0));
    renderer.flush();
    renderer.break_pass();
    rhi::RenderTarget* backdrop = renderer.acquire_offscreen_target({kSize, kSize});
    rhi::RenderTarget* blur = renderer.acquire_offscreen_target({kSize, kSize});
    if (!expect(backdrop != nullptr && blur != nullptr,
                "the pool hands out the two targets the two passes need") ||
        !expect(backdrop != blur, "consecutive acquires in one frame differ (ping-pong)")) {
        return false;
    }
    commands->copy_target_region(*fixture.frame, {0, 0, kSize, kSize}, *backdrop, rhi::Offset2D{});

    gpu::GlassPushConstants constants;
    constants.texel_step_x = 1.0f / static_cast<float>(kSize);
    constants.texel_step_y = constants.texel_step_x;
    constants.sigma = 2.0f;
    constants.radius = 6.0f;

    commands->bind_pipeline(*fixture.glass);
    commands->bind_sampled_target(0, *backdrop);
    commands->begin_render_pass(*blur, desc);
    commands->set_scissor({0, 0, kSize, kSize});
    commands->push_constants(&constants, sizeof(constants));
    commands->draw(kFullscreenVertices);
    commands->end_render_pass();

    constants.mode = 1.0f;
    commands->bind_sampled_target(0, *blur);
    renderer.resume_pass();
    commands->bind_pipeline(*fixture.glass);
    commands->push_constants(&constants, sizeof(constants));
    commands->draw(kFullscreenVertices);
    renderer.end();
    commands->submit();

    const null::NullCommandStats& stats = stats_of(commands);
    const std::size_t copy = index_of(stats, null::NullCommandKind::CopyTargetRegion);
    const std::size_t first_sample = index_of(stats, null::NullCommandKind::BindSampledTarget);
    const std::size_t second_sample =
        last_index_of(stats, null::NullCommandKind::BindSampledTarget);
    // The horizontal pass draws between the two sampled binds; the composite draw follows the
    // second one. Searching from the bind (not from the start) skips the backdrop rectangle draw.
    const auto draw_after = [&stats](std::size_t start) {
        for (std::size_t index = start + 1; index < stats.command_order.size(); ++index) {
            if (stats.command_order[index] == null::NullCommandKind::Draw) {
                return index;
            }
        }
        return stats.command_order.size();
    };
    const std::size_t blur_draw = draw_after(first_sample);
    const std::size_t composite_draw = draw_after(second_sample);
    return expect(stats.region_copies == 1, "the renderer frame copies the backdrop once") &&
           expect(stats.sampled_target_binds == 2, "both blur inputs are sampled") &&
           expect(copy < first_sample, "the copy is recorded before the first sample") &&
           expect(first_sample < blur_draw && blur_draw < second_sample,
                  "the horizontal pass draws between the two sampled binds") &&
           expect(composite_draw < stats.command_order.size(),
                  "the composite draw follows the second sampled bind") &&
           expect(stats.begin_render_passes == 3,
                  "two pooled pass targets plus the resumed frame pass") &&
           expect(stats.end_render_passes == 3, "every pass is closed") &&
           expect(renderer.offscreen_target_count() == 2,
                  "the frame needs exactly two pooled targets") &&
           expect(renderer.draw_calls() == 1, "the renderer counted its own rectangle draw");
}

// A frame that never samples a target records no sampled bind: the no-glass path is unchanged, and
// the new command kind only appears where the effect is used.
bool check_no_glass_frame_has_no_sampled_bind() {
    Fixture fixture;
    gpu::Batched2DRenderer renderer(*fixture.device, *fixture.batched2d, *fixture.white);
    auto commands = fixture.device->create_command_list();
    renderer.begin(*commands, *fixture.frame, target_desc());
    renderer.push_rect({0, 0, 16, 16}, rhi::rgba8(255, 0, 0));
    renderer.end();
    commands->submit();

    const null::NullCommandStats& stats = stats_of(commands);
    return expect(stats.sampled_target_binds == 0, "a no-glass frame records no sampled bind") &&
           expect(stats.command_order.size() == 6,
                  "a no-glass frame is pass, pipeline, texture, draw, end and submit") &&
           expect(stats.texture_binds == 1, "the single batch binds its texture as before") &&
           expect(stats.begin_render_passes == 1 && stats.end_render_passes == 1,
                  "a no-glass frame keeps its single pass");
}

// Readback after sampling: the target's readable state moves between transfer and shader-read, and
// every reader transitions from whichever state it finds. Null records the requests; the pixel
// test proves the Vulkan barriers.
bool check_sample_then_readback() {
    Fixture fixture;
    auto commands = fixture.device->create_command_list();
    commands->begin_render_pass(*fixture.frame, target_desc());
    commands->end_render_pass();

    auto readback = fixture.device->create_buffer(
        {static_cast<std::size_t>(kSize) * kSize * 4, rhi::BufferUsage::Readback});
    commands->bind_pipeline(*fixture.glass);
    commands->bind_sampled_target(0, *fixture.frame);
    commands->copy_target_to_buffer(*fixture.frame, *readback);
    commands->submit();

    const null::NullCommandStats& stats = stats_of(commands);
    return expect(stats.sampled_target_binds == 1 && stats.region_copies == 0,
                  "sampling and readback are separate recorded operations") &&
           expect(stats.draws == 0, "a readback-only list records no draw");
}

} // namespace

int main() {
    if (!check_composite_order()) {
        return 1;
    }
    if (!check_sampler_state_from_desc()) {
        return 2;
    }
    if (!check_invalid_binds_are_rejected()) {
        return 3;
    }
    if (!check_renderer_pooled_composite()) {
        return 4;
    }
    if (!check_no_glass_frame_has_no_sampled_bind()) {
        return 5;
    }
    if (!check_sample_then_readback()) {
        return 6;
    }
    std::cout << "sampled_target: ok\n";
    return 0;
}
