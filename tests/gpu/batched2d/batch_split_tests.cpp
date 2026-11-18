// SPDX-License-Identifier: MIT
//
// Batch-split attribution (issue #480 part 2). The renderer ends a batch for four structural
// reasons plus the caller's own flush; `BatchSplitStats` counts each one, so a frame's draw-call
// count is attributable instead of one opaque number. The issue gates descriptor indexing on how
// much of that count is the texture share, so the counter has to be exact before it is measured.
//
// Runs on the null backend: the counters are renderer state, not GPU state.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <cstdint>
#include <iostream>
#include <memory>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::gpu::BatchSplitStats;
using tessera::rendering::null::create_null_device;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::rhi::rgba8;

constexpr std::uint32_t kSize = 256;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "batch_split: " << message << "\n";
    }
    return condition;
}

// A null-backend renderer with a frame open, plus the textures a split needs.
class Frame {
public:
    Frame() {
        device_ = create_null_device({});
        target_ = device_->create_render_target({{kSize, kSize}});
        white_ = device_->create_texture({{1, 1}});
        other_ = device_->create_texture({{2, 2}});
        third_ = device_->create_texture({{3, 3}});
        pipeline_ = device_->create_pipeline({});
        commands_ = device_->create_command_list();
        renderer_ = std::make_unique<Batched2DRenderer>(*device_, *pipeline_, *white_);
        renderer_->begin(*commands_, *target_, {{kSize, kSize}});
    }

    Batched2DRenderer& renderer() {
        return *renderer_;
    }
    tessera::rendering::rhi::Texture& other() {
        return *other_;
    }
    tessera::rendering::rhi::Texture& third() {
        return *third_;
    }
    void finish() {
        renderer_->end();
        commands_->submit();
    }
    // Opens a second frame on the same renderer, so a test can prove the counters reset.
    void begin_again() {
        renderer_->begin(*commands_, *target_, {{kSize, kSize}});
    }

private:
    std::unique_ptr<tessera::rendering::rhi::Device> device_;
    std::unique_ptr<tessera::rendering::rhi::RenderTarget> target_;
    std::unique_ptr<tessera::rendering::rhi::Texture> white_;
    std::unique_ptr<tessera::rendering::rhi::Texture> other_;
    std::unique_ptr<tessera::rendering::rhi::Texture> third_;
    std::unique_ptr<tessera::rendering::rhi::Pipeline> pipeline_;
    std::unique_ptr<tessera::rendering::rhi::CommandList> commands_;
    std::unique_ptr<Batched2DRenderer> renderer_;
};

// One texture for the whole frame: the single batch ends only at the caller's flush.
int check_single_texture() {
    Frame frame;
    Batched2DRenderer& r = frame.renderer();
    r.push_rect({0, 0, 16, 16}, rgba8(255, 0, 0));
    r.push_rect({16, 0, 16, 16}, rgba8(0, 255, 0));
    r.push_rounded_rect({32, 0, 16, 16}, rgba8(0, 0, 255), 4.0f);
    frame.finish();
    const BatchSplitStats& split = r.batch_split_stats();
    if (!expect(split.texture == 0, "one texture never splits on a texture change") ||
        !expect(split.pipeline == 0, "one pipeline never splits on a pipeline change") ||
        !expect(split.scissor == 0, "an unchanged scissor never splits") ||
        !expect(split.clip == 0, "no clip push never splits") ||
        !expect(split.flush == 1, "the frame's own flush ends the single batch") ||
        !expect(split.total() == 1, "one batch for one texture") ||
        !expect(r.draw_calls() == 1, "the draw count matches the batch count")) {
        return 1;
    }
    return 0;
}

// Alternating textures: every switch is a texture split, and the count is what descriptor indexing
// would remove.
int check_texture_switches() {
    Frame frame;
    Batched2DRenderer& r = frame.renderer();
    const tessera::rendering::rhi::Rect uv{0, 0, 1, 1};
    const auto white = rgba8(255, 255, 255);
    // white -> other -> white -> other -> white: four switches, five batches.
    r.push_rect({0, 0, 8, 8}, white);
    r.push_texture({8, 0, 8, 8}, frame.other(), uv, white);
    r.push_rect({16, 0, 8, 8}, white);
    r.push_texture({24, 0, 8, 8}, frame.other(), uv, white);
    r.push_rect({32, 0, 8, 8}, white);
    frame.finish();
    const BatchSplitStats& split = r.batch_split_stats();
    if (!expect(split.texture == 4, "four texture switches split four times") ||
        !expect(split.total() == 5, "five batches for five runs") ||
        !expect(r.draw_calls() == 5, "the draw count matches the batch count")) {
        return 1;
    }
    return 0;
}

// A run of quads over the same texture stays one batch however many quads it holds.
int check_same_texture_run() {
    Frame frame;
    Batched2DRenderer& r = frame.renderer();
    const tessera::rendering::rhi::Rect uv{0, 0, 1, 1};
    const auto white = rgba8(255, 255, 255);
    for (std::uint32_t index = 0; index < 8; ++index) {
        r.push_texture({static_cast<float>(index) * 8.0f, 0, 8, 8}, frame.other(), uv, white);
    }
    frame.finish();
    const BatchSplitStats& split = r.batch_split_stats();
    if (!expect(split.texture == 0, "eight quads over one texture never split") ||
        !expect(split.total() == 1, "eight quads over one texture are one batch")) {
        return 1;
    }
    return 0;
}

// The scissor and the folded clip set are separate reasons, so a frame that only clips does not
// look like a texture problem.
int check_scissor_and_clip() {
    Frame frame;
    Batched2DRenderer& r = frame.renderer();
    r.push_rect({0, 0, 8, 8}, rgba8(1, 1, 1));
    r.push_scissor({0, 0, 32, 32});
    r.push_rect({8, 0, 8, 8}, rgba8(1, 1, 1));
    r.pop_scissor();
    r.push_rect({16, 0, 8, 8}, rgba8(1, 1, 1));
    frame.finish();
    const BatchSplitStats& split = r.batch_split_stats();
    if (!expect(split.scissor == 2, "push and pop each split on the scissor") ||
        !expect(split.texture == 0, "a scissor split is not a texture split") ||
        !expect(split.clip == 0, "a scissor split is not a clip split")) {
        return 1;
    }

    Frame clipped;
    Batched2DRenderer& c = clipped.renderer();
    c.push_rect({0, 0, 8, 8}, rgba8(1, 1, 1));
    c.push_clip(tessera::rendering::gpu::clip::uniform_rect({0, 0, 32, 32}, 6.0f));
    c.push_rect({8, 0, 8, 8}, rgba8(1, 1, 1));
    c.pop_clip();
    c.push_rect({16, 0, 8, 8}, rgba8(1, 1, 1));
    clipped.finish();
    const BatchSplitStats& clip_split = c.batch_split_stats();
    if (!expect(clip_split.clip == 2, "push and pop each split on the folded clip set") ||
        !expect(clip_split.scissor == 0, "a clip split is not a scissor split") ||
        !expect(clip_split.texture == 0, "a clip split is not a texture split")) {
        return 2;
    }
    return 0;
}

// The counters reset per frame: a second frame does not inherit the first one's splits.
int check_reset() {
    Frame frame;
    Batched2DRenderer& r = frame.renderer();
    r.push_rect({0, 0, 8, 8}, rgba8(1, 1, 1));
    r.push_texture({8, 0, 8, 8}, frame.other(), {0, 0, 1, 1}, rgba8(255, 255, 255));
    frame.finish();
    if (!expect(r.batch_split_stats().texture == 1, "the first frame counts its switch")) {
        return 1;
    }

    frame.begin_again();
    r.push_rect({0, 0, 8, 8}, rgba8(1, 1, 1));
    frame.finish();
    const BatchSplitStats& split = r.batch_split_stats();
    if (!expect(split.texture == 0, "a new frame starts with no texture split") ||
        !expect(split.total() == 1, "the second frame counts only its own batch")) {
        return 2;
    }
    return 0;
}

} // namespace

int main() {
    const int results[] = {check_single_texture(), check_texture_switches(),
                           check_same_texture_run(), check_scissor_and_clip(), check_reset()};
    for (const int result : results) {
        if (result != 0) {
            return result;
        }
    }
    std::cout << "batch_split: ok\n";
    return 0;
}
