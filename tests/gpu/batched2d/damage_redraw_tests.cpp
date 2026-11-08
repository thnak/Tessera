// SPDX-License-Identifier: MIT

#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <cstdint>
#include <iostream>
#include <memory>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::make_redraw_plan;
using tessera::rendering::RedrawPlan;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::null::NullCommandStats;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::Scissor;

constexpr std::uint32_t kWidth = 256;
constexpr std::uint32_t kHeight = 256;

struct FrameResult {
    NullCommandStats stats;
    std::uint32_t draw_calls = 0;
    std::uint32_t vertex_count = 0;
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "damage_redraw: " << message << "\n";
    }
    return condition;
}

bool scissor_equal(const Scissor& lhs, const Scissor& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

bool scissor_inside(const Scissor& inner, const Scissor& outer) {
    const std::int32_t inner_right = inner.x + static_cast<std::int32_t>(inner.width);
    const std::int32_t inner_bottom = inner.y + static_cast<std::int32_t>(inner.height);
    const std::int32_t outer_right = outer.x + static_cast<std::int32_t>(outer.width);
    const std::int32_t outer_bottom = outer.y + static_cast<std::int32_t>(outer.height);
    return inner.x >= outer.x && inner.y >= outer.y && inner_right <= outer_right &&
           inner_bottom <= outer_bottom;
}

void build_scene(Batched2DRenderer& renderer, tessera::rendering::rhi::Texture& checker) {
    renderer.push_rect({0, 0, 256, 256}, rgba8(11, 18, 32));
    renderer.push_rounded_rect({8, 8, 48, 48}, rgba8(56, 189, 248), 8);

    // Entirely outside both damage regions: its batch must not reach the GPU.
    renderer.push_scissor({200, 200, 20, 20});
    renderer.push_rect({200, 200, 20, 20}, rgba8(250, 204, 21));
    renderer.pop_scissor();

    // Narrower than the damage region: the emitted scissor keeps this clip.
    renderer.push_scissor({32, 32, 32, 32});
    renderer.push_rect({0, 0, 256, 256}, rgba8(30, 41, 59));
    renderer.pop_scissor();

    renderer.push_texture({140, 64, 32, 32}, checker, {0, 0, 1, 1}, rgba8(255, 255, 255));
}

FrameResult run_frame(const RedrawPlan* plan) {
    using namespace tessera::rendering;

    auto device = null::create_null_device(rhi::DeviceDesc{});
    auto target = device->create_render_target(rhi::RenderTargetDesc{{kWidth, kHeight}});
    auto white = device->create_texture(rhi::TextureDesc{{1, 1}});
    auto checker = device->create_texture(rhi::TextureDesc{{2, 2}});
    auto pipeline = device->create_pipeline(rhi::PipelineDesc{});
    auto commands = device->create_command_list();

    gpu::Batched2DRenderer renderer(*device, *pipeline, *white);
    const rhi::RenderTargetDesc target_desc{{kWidth, kHeight}};
    if (plan != nullptr) {
        renderer.begin_damaged(*commands, *target, target_desc, *plan);
    } else {
        renderer.begin(*commands, *target, target_desc);
    }
    build_scene(renderer, *checker);
    renderer.end();
    commands->submit();

    FrameResult result;
    result.draw_calls = renderer.draw_calls();
    result.vertex_count = renderer.vertex_count();
    const auto* null_commands = dynamic_cast<const NullCommandList*>(commands.get());
    if (null_commands != nullptr) {
        result.stats = null_commands->stats();
    }
    return result;
}

RedrawPlan make_partial_plan() {
    DamageAccumulator damage(DamageConfig{Extent2D{kWidth, kHeight}, 4});
    damage.add(Rect{0, 0, 64, 64});
    damage.add(Rect{128, 128, 64, 64});
    return make_redraw_plan(damage);
}

int check_full_equivalence() {
    RedrawPlan full_plan;
    full_plan.full_redraw = true;
    full_plan.scissors.push_back(Scissor{0, 0, kWidth, kHeight});

    const FrameResult plain = run_frame(nullptr);
    const FrameResult damaged = run_frame(&full_plan);

    if (!expect(plain.stats.draws == 4 && plain.stats.vertices == 30, "plain baseline") ||
        !expect(damaged.stats.draws == plain.stats.draws, "full-plan draws") ||
        !expect(damaged.stats.vertices == plain.stats.vertices, "full-plan vertices") ||
        !expect(damaged.stats.scissor_changes == plain.stats.scissor_changes,
                "full-plan scissor changes") ||
        !expect(damaged.draw_calls == plain.draw_calls, "full-plan renderer draws") ||
        !expect(damaged.vertex_count == plain.vertex_count, "full-plan renderer vertices") ||
        !expect(damaged.stats.draw_records.size() == plain.stats.draw_records.size(),
                "full-plan record count")) {
        return 1;
    }
    for (std::size_t i = 0; i < plain.stats.draw_records.size(); ++i) {
        const auto& plain_record = plain.stats.draw_records[i];
        const auto& damaged_record = damaged.stats.draw_records[i];
        if (!expect(scissor_equal(plain_record.scissor, damaged_record.scissor),
                    "full-plan record scissor") ||
            !expect(plain_record.vertex_count == damaged_record.vertex_count,
                    "full-plan record vertices") ||
            !expect(plain_record.first_vertex == damaged_record.first_vertex,
                    "full-plan record offset")) {
            return 2;
        }
    }
    return 0;
}

int check_partial_containment() {
    const RedrawPlan plan = make_partial_plan();
    if (!expect(!plan.full_redraw, "partial plan flag") ||
        !expect(plan.scissors.size() == 2, "partial plan region count")) {
        return 1;
    }

    const FrameResult frame = run_frame(&plan);
    const Scissor& region_a = plan.scissors[0];
    const Scissor& region_b = plan.scissors[1];

    if (!expect(frame.stats.draws == 5, "partial draw count") ||
        !expect(frame.stats.vertices == 42, "partial vertex count") ||
        !expect(frame.draw_calls == 5, "partial renderer draws") ||
        !expect(frame.stats.draw_records.size() == 5, "partial record count")) {
        return 2;
    }

    for (const auto& record : frame.stats.draw_records) {
        if (!expect(scissor_inside(record.scissor, region_a) ||
                        scissor_inside(record.scissor, region_b),
                    "draw scissor outside damage regions")) {
            return 3;
        }
    }

    // Batch 1 (background) is replayed once per region; the narrow clip keeps its own
    // bounds; the off-damage batch produced no draw at all.
    const auto& records = frame.stats.draw_records;
    if (!expect(scissor_equal(records[0].scissor, region_a), "region a replay") ||
        !expect(scissor_equal(records[1].scissor, region_b), "region b replay") ||
        !expect(scissor_equal(records[2].scissor, Scissor{32, 32, 32, 32}),
                "narrow clip preserved") ||
        !expect(scissor_equal(records[3].scissor, region_a), "textured region a replay") ||
        !expect(scissor_equal(records[4].scissor, region_b), "textured region b replay")) {
        return 4;
    }
    return 0;
}

int check_empty_noop() {
    const RedrawPlan empty_plan;
    const FrameResult frame = run_frame(&empty_plan);

    if (!expect(frame.stats.begin_render_passes == 0, "empty plan begins no pass") ||
        !expect(frame.stats.end_render_passes == 0, "empty plan ends no pass") ||
        !expect(frame.stats.draws == 0, "empty plan draws nothing") ||
        !expect(frame.stats.draw_records.empty(), "empty plan records nothing") ||
        !expect(frame.draw_calls == 0, "empty plan renderer draws") ||
        !expect(frame.vertex_count == 0, "empty plan emits no vertices")) {
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    const int full = check_full_equivalence();
    if (full != 0) {
        std::cerr << "damage_redraw: full-plan equivalence failed (" << full << ")\n";
        return 1;
    }
    const int partial = check_partial_containment();
    if (partial != 0) {
        std::cerr << "damage_redraw: partial-plan containment failed (" << partial << ")\n";
        return 2;
    }
    const int empty = check_empty_noop();
    if (empty != 0) {
        std::cerr << "damage_redraw: empty-plan no-op failed (" << empty << ")\n";
        return 3;
    }

    std::cout << "damage_redraw: ok\n";
    return 0;
}
