// SPDX-License-Identifier: MIT
//
// CPU culling in Batched2DRenderer (issue #474): a quad that cannot touch the active scissor or the
// damage regions is never staged. Runs on the null backend, so it checks the counts and the batch
// structure; the pixels are pinned by the damage equivalence tests, which render with and without
// damage on the CPU canvas.

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
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Mat3;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::Scissor;

constexpr std::uint32_t kSize = 256;
constexpr std::uint32_t kQuad = 6;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "cull: " << message << "\n";
    }
    return condition;
}

// A null-backend renderer with a frame open.
class Frame {
public:
    explicit Frame(bool cull = true, const RedrawPlan* plan = nullptr) {
        using namespace tessera::rendering;
        device_ = null::create_null_device(rhi::DeviceDesc{});
        target_ = device_->create_render_target(rhi::RenderTargetDesc{{kSize, kSize}});
        white_ = device_->create_texture(rhi::TextureDesc{{1, 1}});
        other_ = device_->create_texture(rhi::TextureDesc{{2, 2}});
        pipeline_ = device_->create_pipeline(rhi::PipelineDesc{});
        commands_ = device_->create_command_list();
        Batched2DConfig config;
        config.cull_draws = cull;
        renderer_ = std::make_unique<Batched2DRenderer>(*device_, *pipeline_, *white_, config);
        const rhi::RenderTargetDesc desc{{kSize, kSize}};
        if (plan != nullptr) {
            renderer_->begin_damaged(*commands_, *target_, desc, *plan);
        } else {
            renderer_->begin(*commands_, *target_, desc);
        }
    }

    Batched2DRenderer& renderer() {
        return *renderer_;
    }
    tessera::rendering::rhi::Texture& other() {
        return *other_;
    }
    void finish() {
        renderer_->end();
        commands_->submit();
    }

private:
    std::unique_ptr<tessera::rendering::rhi::Device> device_;
    std::unique_ptr<tessera::rendering::rhi::RenderTarget> target_;
    std::unique_ptr<tessera::rendering::rhi::Texture> white_;
    std::unique_ptr<tessera::rendering::rhi::Texture> other_;
    std::unique_ptr<tessera::rendering::rhi::Pipeline> pipeline_;
    std::unique_ptr<tessera::rendering::rhi::CommandList> commands_;
    std::unique_ptr<Batched2DRenderer> renderer_;
};

int check_viewport() {
    Frame frame;
    Batched2DRenderer& r = frame.renderer();
    r.push_rect({10, 10, 20, 20}, rgba8(255, 0, 0));       // inside
    r.push_rect({-100, 10, 20, 20}, rgba8(0, 255, 0));     // left of the target
    r.push_rect({10, 300, 20, 20}, rgba8(0, 0, 255));      // below it
    r.push_rect({kSize - 10, 10, 40, 20}, rgba8(9, 9, 9)); // straddles the right edge: kept
    r.push_rect({kSize, 0, 20, 20}, rgba8(9, 9, 9)); // touches the edge only: kept (conservative)
    if (!expect(r.cull_stats().quads_emitted == 3, "three quads reach the buffer") ||
        !expect(r.cull_stats().quads_culled == 2, "two are culled") ||
        !expect(r.cull_stats().quads_culled_empty_scissor == 0, "no empty-scissor cull") ||
        !expect(r.vertex_count() == 3 * kQuad, "only kept quads have vertices")) {
        return 1;
    }
    frame.finish();
    return 0;
}

int check_switch_off() {
    Frame frame(false);
    Batched2DRenderer& r = frame.renderer();
    r.push_rect({10, 10, 20, 20}, rgba8(255, 0, 0));
    r.push_rect({-100, 10, 20, 20}, rgba8(0, 255, 0));
    if (!expect(r.cull_stats().quads_culled == 0 && r.vertex_count() == 2 * kQuad,
                "cull_draws=false keeps every quad")) {
        return 1;
    }
    frame.finish();
    return 0;
}

int check_scissor() {
    Frame frame;
    Batched2DRenderer& r = frame.renderer();
    r.push_scissor({0, 0, 50, 50});
    r.push_rect({10, 10, 10, 10}, rgba8(1, 1, 1));   // inside the scissor
    r.push_rect({100, 100, 10, 10}, rgba8(1, 1, 1)); // inside the target, outside the scissor
    r.pop_scissor();
    r.push_rect({100, 100, 10, 10}, rgba8(1, 1, 1)); // the scissor is gone: kept
    if (!expect(r.cull_stats().quads_emitted == 2 && r.cull_stats().quads_culled == 1,
                "a quad outside the pushed scissor is culled, and only there")) {
        return 1;
    }

    // Two scissors that do not meet leave nothing to draw: everything is dropped and counted as an
    // empty-scissor cull, whatever the quad covers.
    r.push_scissor({0, 0, 20, 20});
    r.push_scissor({100, 100, 20, 20});
    r.push_rect({0, 0, kSize, kSize}, rgba8(2, 2, 2));
    r.pop_scissor();
    r.pop_scissor();
    if (!expect(r.cull_stats().quads_culled_empty_scissor == 1,
                "an empty scissor drops the quad") ||
        !expect(r.cull_stats().quads_culled == 2, "the empty-scissor drop is also a cull")) {
        return 2;
    }
    frame.finish();
    return 0;
}

int check_damage() {
    DamageAccumulator damage(DamageConfig{Extent2D{kSize, kSize}, 4});
    damage.add(Rect{0, 0, 64, 64});
    damage.add(Rect{128, 128, 64, 64});
    const RedrawPlan plan = make_redraw_plan(damage);
    if (!expect(!plan.full_redraw && plan.scissors.size() == 2, "a partial plan")) {
        return 1;
    }
    Frame frame(true, &plan);
    Batched2DRenderer& r = frame.renderer();
    r.push_rect({8, 8, 16, 16}, rgba8(1, 1, 1));     // in region 1
    r.push_rect({140, 140, 16, 16}, rgba8(1, 1, 1)); // in region 2
    r.push_rect({70, 70, 50, 50}, rgba8(1, 1, 1));   // in the bounding box, in no region: kept
    r.push_rect({200, 10, 30, 30}, rgba8(1, 1, 1));  // outside the bounding box
    r.push_rect({10, 200, 30, 30}, rgba8(1, 1, 1));  // outside the bounding box
    // The in-box quad is the conservative case: the bounding box is a superset of the regions, and
    // flush() still drops its batch per region. Only quads outside the box are culled here.
    if (!expect(r.cull_stats().quads_emitted == 3 && r.cull_stats().quads_culled == 2,
                "quads outside every damage region's bounding box are culled")) {
        return 2;
    }
    frame.finish();
    return 0;
}

int check_transform() {
    Frame frame;
    Batched2DRenderer& r = frame.renderer();
    r.push_transform(Mat3::translation(300, 0)); // moves a quad at x 0 off the target
    r.push_rect({0, 0, 20, 20}, rgba8(1, 1, 1));
    r.pop_transform();
    r.push_transform(Mat3::translation(-10, 0));
    r.push_rect({0, 0, 20, 20}, rgba8(1, 1, 1)); // half on the target: kept
    r.pop_transform();
    // A rotation by 45 degrees about the origin of a quad centred on the target stays inside the
    // target, and its axis-aligned box is what is tested.
    r.push_transform(Mat3::translation(128, 128) * Mat3::rotation(0.785398f));
    r.push_rect({-20, -20, 40, 40}, rgba8(1, 1, 1));
    r.pop_transform();
    if (!expect(r.cull_stats().quads_emitted == 2 && r.cull_stats().quads_culled == 1,
                "the transform is applied before the test")) {
        return 1;
    }
    frame.finish();
    return 0;
}

int check_content_scale() {
    Frame frame;
    Batched2DRenderer& r = frame.renderer();
    r.set_content_scale(0.5f, 0.5f);
    r.push_rect({300, 10, 20, 20}, rgba8(1, 1, 1)); // 150 on the target: kept
    r.push_rect({600, 10, 20, 20}, rgba8(1, 1, 1)); // 300 on the target: culled
    if (!expect(r.cull_stats().quads_emitted == 1 && r.cull_stats().quads_culled == 1,
                "the content scale is applied before the test")) {
        return 1;
    }
    frame.finish();
    return 0;
}

int check_batches_not_split() {
    // A culled quad with another texture must not end the batch of its neighbours.
    Frame frame;
    Batched2DRenderer& r = frame.renderer();
    r.push_rect({10, 10, 20, 20}, rgba8(1, 1, 1));
    r.push_texture({-200, 0, 20, 20}, frame.other(), {0, 0, 1, 1}, rgba8(255, 255, 255));
    r.push_rect({40, 10, 20, 20}, rgba8(1, 1, 1));
    frame.finish();
    if (!expect(r.draw_calls() == 1, "culling a quad does not split the batch")) {
        return 1;
    }
    return 0;
}

int check_stats_reset() {
    Frame frame;
    Batched2DRenderer& r = frame.renderer();
    r.push_rect({-100, 0, 20, 20}, rgba8(1, 1, 1));
    frame.finish();
    if (!expect(r.cull_stats().quads_culled == 1, "the culled quad is counted")) {
        return 1;
    }
    return 0;
}

} // namespace

int main() {
    const int results[] = {
        check_viewport(),  check_switch_off(),    check_scissor(),           check_damage(),
        check_transform(), check_content_scale(), check_batches_not_split(), check_stats_reset()};
    for (int result : results) {
        if (result != 0) {
            return result;
        }
    }
    std::cout << "cull: ok\n";
    return 0;
}
