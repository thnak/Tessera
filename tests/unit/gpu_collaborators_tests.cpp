// SPDX-License-Identifier: MIT
//
// The three state clusters extracted from Batched2DRenderer by the god-type audit slice 8, tested
// directly instead of through a command stream:
//
//  - ClipStateTable folds a clip stack into shader slots, interns the fold, and reports the scissor
//    a push/pop implies (including the deeper-than-kMaxClipDepth fold and the unbalanced pop).
//  - VertexArena rotates its buffer slots, appends within a frame and grows past a slot's capacity
//    without losing earlier ranges.
//  - GlassCompositor reports what it needs before it can run, and records nothing when it cannot.

#include "tessera/rendering/gpu/clip_state_table.hpp"
#include "tessera/rendering/gpu/glass_compositor.hpp"
#include "tessera/rendering/gpu/vertex_arena.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <cstdint>
#include <iostream>
#include <memory>

namespace {

using namespace tessera::rendering;

namespace clip = tessera::rendering::gpu::clip;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "gpu_collaborators: " << message << "\n";
    }
    return condition;
}

constexpr rhi::Scissor kBaseScissor{0, 0, 128, 128};

clip::RoundedRect rect_clip(float x, float y, float w, float h) {
    return clip::make_rounded_rect(rhi::Rect{x, y, w, h}, 0.0f, 0.0f, 0.0f, 0.0f);
}

// --- ClipStateTable ---------------------------------------------------------------------------

bool check_clip_table_starts_empty() {
    gpu::ClipStateTable table;
    return expect(table.index() == 0, "the empty clip set is set 0") &&
           expect(table.depth() == 0, "a fresh table has no depth") &&
           expect(table.empty(), "a fresh table is empty") &&
           expect(table.interned_count() == 1, "only the empty set is interned at first") &&
           expect(table.set(0).empty(), "set 0 carries no clip records");
}

bool check_clip_table_push_pop_restores_scissor() {
    gpu::ClipStateTable table;
    table.reset();

    const auto pushed = table.push(rect_clip(10.0f, 10.0f, 40.0f, 40.0f), kBaseScissor);
    if (!expect(pushed.set != 0, "a pushed clip interns a new set") ||
        !expect(table.depth() == 1, "push adds one level") ||
        !expect(table.set(pushed.set).size() == 1, "one active clip folds into one record")) {
        return false;
    }
    // The enclosing scissor is the intersection of the current scissor and the clip's rect.
    if (!expect(pushed.scissor.x == 10 && pushed.scissor.y == 10 && pushed.scissor.width == 40 &&
                    pushed.scissor.height == 40,
                "push reports the clip's enclosing scissor")) {
        return false;
    }

    const auto popped = table.pop(kBaseScissor);
    return expect(popped.set == 0, "popping returns to the empty set") &&
           expect(table.depth() == 0, "pop removes the level") &&
           expect(popped.scissor.x == kBaseScissor.x && popped.scissor.y == kBaseScissor.y &&
                      popped.scissor.width == kBaseScissor.width &&
                      popped.scissor.height == kBaseScissor.height,
                  "pop restores the scissor the push was entered with");
}

bool check_clip_table_reuses_interned_sets() {
    gpu::ClipStateTable table;
    table.reset();
    const auto first = table.push(rect_clip(10.0f, 10.0f, 40.0f, 40.0f), kBaseScissor);
    static_cast<void>(table.pop(kBaseScissor)); // the popped value is checked elsewhere
    const std::uint32_t after_pop = table.interned_count();
    const auto again = table.push(rect_clip(10.0f, 10.0f, 40.0f, 40.0f), kBaseScissor);
    return expect(again.set == first.set,
                  "the same fold interns to the same index, so a batch does not split") &&
           expect(table.interned_count() == after_pop,
                  "re-pushing an identical clip interns nothing new");
}

bool check_clip_table_keeps_every_clip_as_a_record() {
    gpu::ClipStateTable table;
    table.reset();
    // Since ADR-068 slice 3 the shader reads a record list, so there is no three-slot ceiling: five
    // distinct clips are five records, and the stack bound (kMaxClipDepth) is what limits nesting.
    std::uint32_t last_set = 0;
    rhi::Scissor scissor = kBaseScissor;
    for (std::uint32_t i = 0; i < 5; ++i) {
        const auto update =
            table.push(rect_clip(static_cast<float>(i), 0.0f, 100.0f, 100.0f), scissor);
        last_set = update.set;
        scissor = update.scissor;
    }
    if (!expect(table.depth() == 5, "five clips fit under kMaxClipDepth") ||
        !expect(table.set(last_set).size() == 5, "every clip becomes its own record")) {
        return false;
    }
    // The records are in stack order, so the last one is the innermost clip, unmodified: the fold
    // that used to intersect past the third slot no longer happens.
    const clip::ClipRecord& innermost = table.set(last_set).back();
    return expect(innermost.x == 4.0f && innermost.width == 100.0f,
                  "the innermost record is the clip as pushed, not a folded intersection");
}

bool check_clip_table_folds_past_the_stack_bound() {
    gpu::ClipStateTable table;
    table.reset();
    // kMaxClipDepth is the stack bound; a clip past it folds into the innermost level
    // conservatively, which is the documented nesting rule.
    rhi::Scissor scissor = kBaseScissor;
    std::uint32_t last_set = 0;
    for (std::uint32_t i = 0; i < clip::kMaxClipDepth + 2; ++i) {
        const auto update =
            table.push(rect_clip(static_cast<float>(i), 0.0f, 100.0f, 100.0f), scissor);
        last_set = update.set;
        scissor = update.scissor;
    }
    if (!expect(table.depth() == clip::kMaxClipDepth, "the stack stops at kMaxClipDepth") ||
        !expect(table.set(last_set).size() == clip::kMaxClipDepth,
                "the fold keeps one record per stack level")) {
        return false;
    }
    // The two clips past the bound intersect into the innermost level. Level 31 holds the clip at
    // x=31 (right edge 131); the clips at x=32 and x=33 fold into it, so the result starts at 33
    // and keeps the right edge 131: width 98.
    const clip::ClipRecord& innermost = table.set(last_set).back();
    return expect(innermost.x == static_cast<float>(clip::kMaxClipDepth + 1),
                  "clips past the bound fold into the innermost level") &&
           expect(innermost.width == 100.0f - 2.0f,
                  "the folded record is the conservative intersection");
}

bool check_clip_table_unbalanced_pop_is_a_no_op() {
    gpu::ClipStateTable table;
    table.reset();
    const auto popped = table.pop(kBaseScissor);
    return expect(popped.set == 0, "an unbalanced pop keeps the empty set") &&
           expect(popped.scissor.x == kBaseScissor.x && popped.scissor.width == kBaseScissor.width,
                  "an unbalanced pop keeps the caller's scissor");
}

bool check_clip_table_packs_push_constants() {
    gpu::ClipStateTable table;
    table.reset();
    // make_rounded_rect clamps every radius to min(width, height) / 2, so a 20x20 box caps the
    // radii at 10: use values under that to observe them unclamped.
    const auto pushed = table.push(
        clip::make_rounded_rect(rhi::Rect{5.0f, 6.0f, 20.0f, 20.0f}, 1.0f, 2.0f, 3.0f, 4.0f),
        kBaseScissor);
    gpu::Batched2DPushConstants push;
    table.pack_push_constants(pushed.set, rhi::Extent2D{320, 200}, 1.0f, push);
    // The block carries the viewport, the aa radius and the set index; the clip geometry is in the
    // record buffer now (ADR-068 slice 3).
    const auto records = table.set(pushed.set);
    return expect(push.viewport_width == 320.0f && push.viewport_height == 200.0f,
                  "the viewport comes from the frame extent") &&
           expect(push.aa_radius == 1.0f, "the aa radius is passed through") &&
           expect(push.clip_set == pushed.set, "the interned set index is packed") &&
           expect(records.size() == 1, "one clip is one record") &&
           expect(records[0].x == 5.0f && records[0].y == 6.0f && records[0].width == 20.0f &&
                      records[0].height == 20.0f,
                  "the clip rect is in the record") &&
           expect(records[0].radii[0] == 1.0f && records[0].radii[1] == 2.0f &&
                      records[0].radii[2] == 3.0f && records[0].radii[3] == 4.0f,
                  "the four corner radii are in the record") &&
           expect(records[0].kind == static_cast<std::uint32_t>(clip::RecordKind::RoundedRect),
                  "slice 3 emits RoundedRect records");
}

// --- VertexArena ------------------------------------------------------------------------------

struct Fixture {
    std::unique_ptr<rhi::Device> device = null::create_null_device(rhi::DeviceDesc{});
};

gpu::Vertex2D vertex(float x) {
    gpu::Vertex2D out;
    out.x = x;
    return out;
}

bool check_arena_appends_within_a_frame() {
    Fixture fixture;
    gpu::VertexArena arena(*fixture.device, 64, 1);
    arena.begin_frame();
    arena.push_vertex(vertex(1.0f));
    arena.push_vertex(vertex(2.0f));
    const auto first = arena.upload();
    if (!expect(first.base_vertex == 0, "the first upload starts at vertex 0") ||
        !expect(first.vertex_count == 2, "the first upload covers both staged vertices") ||
        !expect(first.buffer != nullptr, "the first upload names a buffer")) {
        return false;
    }
    arena.clear();

    arena.push_vertex(vertex(3.0f));
    const auto second = arena.upload();
    return expect(second.buffer == first.buffer,
                  "a second flush of the same frame reuses the buffer") &&
           expect(second.base_vertex == 2,
                  "a second flush appends after the first instead of overwriting it") &&
           expect(arena.vertex_count() == 3, "vertex_count totals the frame, not the flush");
}

bool check_arena_grows_without_losing_earlier_ranges() {
    Fixture fixture;
    // A tiny capacity forces a growth on the second upload.
    gpu::VertexArena arena(*fixture.device, 64, 1);
    arena.begin_frame();
    for (int i = 0; i < 60; ++i) {
        arena.push_vertex(vertex(static_cast<float>(i)));
    }
    const auto first = arena.upload();
    arena.clear();

    for (int i = 0; i < 60; ++i) {
        arena.push_vertex(vertex(static_cast<float>(i)));
    }
    const auto second = arena.upload();
    return expect(second.buffer != first.buffer, "outgrowing the buffer continues in a new one") &&
           expect(first.buffer != nullptr,
                  "the earlier buffer is still referenced, not destroyed") &&
           expect(second.base_vertex == 0, "the grown buffer starts at vertex 0") &&
           expect(second.vertex_count == 60, "the grown upload covers the whole staged range");
}

bool check_arena_rotates_slots() {
    Fixture fixture;
    gpu::VertexArena arena(*fixture.device, 64, 2);
    arena.begin_frame();
    arena.push_vertex(vertex(1.0f));
    const auto frame_a = arena.upload();
    arena.clear();

    arena.begin_frame();
    arena.push_vertex(vertex(2.0f));
    const auto frame_b = arena.upload();
    if (!expect(frame_b.buffer != frame_a.buffer, "two frames in flight use different buffers")) {
        return false;
    }
    arena.clear();

    arena.begin_frame();
    arena.push_vertex(vertex(3.0f));
    const auto frame_c = arena.upload();
    return expect(frame_c.buffer == frame_a.buffer,
                  "the third frame reuses the first slot after the window rotates");
}

bool check_arena_cancel_does_not_consume_a_slot() {
    Fixture fixture;
    gpu::VertexArena arena(*fixture.device, 64, 2);
    arena.begin_frame();
    arena.push_vertex(vertex(1.0f));
    const auto first = arena.upload();
    arena.clear();

    // A no-op frame must not rotate: a frame the GPU never saw should not age the in-flight window.
    arena.cancel_frame();
    arena.begin_frame();
    arena.push_vertex(vertex(2.0f));
    const auto second = arena.upload();
    return expect(second.buffer != first.buffer, "the next real frame still rotates once") &&
           expect(arena.vertex_count() == 1, "cancel reset the frame's vertex count");
}

// --- GlassCompositor --------------------------------------------------------------------------

bool check_compositor_reports_unsupported_without_pipelines() {
    Fixture fixture;
    gpu::GlassCompositor compositor(*fixture.device, gpu::GlassPipelines{},
                                    gpu::OffscreenTargetPoolConfig{4}, gpu::GlassLimits{});
    return expect(!compositor.supported(), "no pipelines means unsupported") &&
           expect(compositor.target_count() == 0, "no pool when glass is not configured");
}

bool check_compositor_reports_unsupported_without_pool() {
    Fixture fixture;
    auto blur = fixture.device->create_pipeline(rhi::PipelineDesc{});
    auto composite = fixture.device->create_pipeline(rhi::PipelineDesc{});
    gpu::GlassPipelines pipelines{blur.get(), composite.get()};
    gpu::GlassCompositor compositor(*fixture.device, pipelines, gpu::OffscreenTargetPoolConfig{0},
                                    gpu::GlassLimits{});
    return expect(!compositor.supported(),
                  "max_targets == 0 disables the pool and therefore the effect") &&
           expect(compositor.target_count() == 0, "a disabled pool holds no targets");
}

bool check_compositor_supported_with_pipelines_and_pool() {
    Fixture fixture;
    auto blur = fixture.device->create_pipeline(rhi::PipelineDesc{});
    auto composite = fixture.device->create_pipeline(rhi::PipelineDesc{});
    gpu::GlassPipelines pipelines{blur.get(), composite.get()};
    gpu::GlassCompositor compositor(*fixture.device, pipelines, gpu::OffscreenTargetPoolConfig{4},
                                    gpu::GlassLimits{});
    compositor.begin_frame(1, 1, rhi::TextureFormat::R8G8B8A8Unorm);
    return expect(compositor.supported(), "pipelines and a pool mean supported") &&
           expect(compositor.draws() == 0, "a fresh frame records no composite");
}

bool check_compositor_rejects_a_pass_through_sigma() {
    Fixture fixture;
    auto blur = fixture.device->create_pipeline(rhi::PipelineDesc{});
    auto composite = fixture.device->create_pipeline(rhi::PipelineDesc{});
    gpu::GlassCompositor compositor(*fixture.device,
                                    gpu::GlassPipelines{blur.get(), composite.get()},
                                    gpu::OffscreenTargetPoolConfig{4}, gpu::GlassLimits{});
    compositor.begin_frame(1, 1, rhi::TextureFormat::R8G8B8A8Unorm);

    auto target = fixture.device->create_render_target(
        rhi::RenderTargetDesc{rhi::Extent2D{64, 64}, rhi::TextureFormat::R8G8B8A8Unorm});
    auto commands = fixture.device->create_command_list();

    gpu::GlassCompositor::FrameContext frame;
    frame.commands = commands.get();
    frame.target = target.get();
    frame.extent = rhi::Extent2D{64, 64};
    frame.scissor = rhi::Scissor{0, 0, 64, 64};

    gpu::GlassBackdropDraw draw;
    draw.clip = clip::uniform_rect(rhi::Rect{8.0f, 8.0f, 32.0f, 32.0f}, 4.0f);
    draw.sigma = 0.0f; // pass-through: nothing to blur

    return expect(!compositor.compose(frame, draw),
                  "a zero sigma is a documented no-op, not a composite") &&
           expect(compositor.draws() == 0, "a rejected composite counts no draw");
}

} // namespace

int main() {
    bool ok = true;
    ok = check_clip_table_starts_empty() && ok;
    ok = check_clip_table_push_pop_restores_scissor() && ok;
    ok = check_clip_table_reuses_interned_sets() && ok;
    ok = check_clip_table_keeps_every_clip_as_a_record() && ok;
    ok = check_clip_table_folds_past_the_stack_bound() && ok;
    ok = check_clip_table_unbalanced_pop_is_a_no_op() && ok;
    ok = check_clip_table_packs_push_constants() && ok;
    ok = check_arena_appends_within_a_frame() && ok;
    ok = check_arena_grows_without_losing_earlier_ranges() && ok;
    ok = check_arena_rotates_slots() && ok;
    ok = check_arena_cancel_does_not_consume_a_slot() && ok;
    ok = check_compositor_reports_unsupported_without_pipelines() && ok;
    ok = check_compositor_reports_unsupported_without_pool() && ok;
    ok = check_compositor_supported_with_pipelines_and_pool() && ok;
    ok = check_compositor_rejects_a_pass_through_sigma() && ok;
    if (!ok) {
        std::cerr << "gpu_collaborators: FAILED\n";
        return 1;
    }
    std::cout << "gpu_collaborators: all checks passed\n";
    return 0;
}
