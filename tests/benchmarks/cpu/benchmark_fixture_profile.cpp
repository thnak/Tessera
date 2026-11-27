// SPDX-License-Identifier: MIT
//
// The stage profile (`tessera_benchmark --stage-profile`): one frame per reference scene recorded
// the way the window does it, with every stage timed and every counter read from the engine's own
// stats.

#include "benchmark_fixture.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <chrono>
#include <vector>

namespace tessera::benchmark {
namespace {

using Clock = std::chrono::steady_clock;
namespace nullrhi = tessera::rendering::null;

[[nodiscard]] double ms_between(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

constexpr auto idx(Stage stage) {
    return static_cast<std::size_t>(stage);
}

} // namespace

SceneProfile Fixture::profile_frame(const char* name, bool damaged) {
    SceneProfile result;
    result.name = name;
    const FrameWork frame = driver_->begin_frame();
    const FramePhases& phases = driver_->phases();
    result.ms[idx(Stage::Tick)] = phases.tick_ms;
    result.ms[idx(Stage::Style)] = phases.styles_ms + phases.sync_style_ms;
    result.ms[idx(Stage::Layout)] =
        phases.layout_input_ms + phases.layout_engine_ms + phases.apply_layout_ms;
    result.ms[idx(Stage::Damage)] = phases.finish_ms;

    const rendering::RedrawPlan plan = frame.render.redraw_plan;
    const bool partial = damaged && !plan.full_redraw;
    // A fresh list per frame, outside the timed region: a real device needs one per submission, and
    // on the null backend it makes the recorded-command stats exactly this frame's.
    commands_ = device_->create_command_list();
    const rhi::RenderTargetDesc desc{{kTargetWidth, kTargetHeight}};
    sink_.reset();
    sink_.set_timing(true);
    const Clock::time_point begin = Clock::now();
    if (partial) {
        renderer_->begin_damaged(*commands_, *target_, desc, plan);
    } else {
        renderer_->begin(*commands_, *target_, desc);
    }
    const Clock::time_point begun = Clock::now();
    const ui::frame::FramePaint painted = driver_->paint(sink_, false);
    const Clock::time_point painted_at = Clock::now();
    renderer_->end();
    const Clock::time_point flushed = Clock::now();
    sink_.set_timing(false);
    if (options_.backend == BackendKind::Vulkan) {
        commands_->submit();
        result.ms[idx(Stage::Submit)] = ms_between(flushed, Clock::now());
        result.submit_measured = true;
        result.command_stats = false;
    }
    if (!painted.painted) {
        throw std::runtime_error("benchmark: the profiled frame was not painted");
    }
    const double paint_total = ms_between(begun, painted_at);
    result.ms[idx(Stage::Begin)] = ms_between(begin, begun);
    result.ms[idx(Stage::SinkBatch)] = sink_.sink_ms();
    result.ms[idx(Stage::PaintPass)] =
        paint_total > sink_.sink_ms() ? paint_total - sink_.sink_ms() : 0.0;
    result.ms[idx(Stage::Flush)] = ms_between(painted_at, flushed);

    StageCounters& c = result.counters;
    c.paint_nodes = painted.stats.nodes;
    c.culled_nodes = painted.stats.culled_nodes;
    c.culled_subtrees = painted.stats.culled_subtrees;
    const DrawListCounts& counts = sink_.counts();
    c.primitives = counts.primitives();
    c.text_runs = counts.text_runs;
    c.text_glyphs = counts.text_glyphs;
    c.text_glyph_bytes_est = counts.text_glyphs * 6u * sizeof(rendering::gpu::Vertex2D);
    c.images = counts.images;
    c.gradients = counts.gradients;
    c.clips = counts.clips;
    c.transforms = counts.transforms;
    c.quads = renderer_->vertex_count() / 6u;
    c.quads_culled = renderer_->cull_stats().quads_culled;
    c.vertex_bytes =
        static_cast<std::uint64_t>(renderer_->vertex_count()) * sizeof(rendering::gpu::Vertex2D);
    c.clip_upload_bytes = renderer_->clip_buffer().uploaded_bytes();
    c.clip_upload_calls = renderer_->clip_buffer().upload_calls();
    c.draw_calls = renderer_->draw_calls();
    const rendering::gpu::BatchSplitStats& splits = renderer_->batch_split_stats();
    c.split_texture = splits.texture;
    c.split_pipeline = splits.pipeline;
    c.split_scissor = splits.scissor;
    c.split_clip = splits.clip;
    c.split_flush = splits.flush;
    if (options_.backend == BackendKind::Null) {
        const nullrhi::NullCommandStats& stats =
            static_cast<const nullrhi::NullCommandList&>(*commands_).stats();
        c.render_passes = stats.begin_render_passes;
        c.pipeline_binds = stats.pipeline_binds;
        c.texture_binds = stats.texture_binds;
        c.vertex_buffer_binds = stats.vertex_buffer_binds;
        c.storage_buffer_binds = stats.storage_buffer_binds;
        c.scissor_changes = stats.scissor_changes;
        c.sampled_target_binds = stats.sampled_target_binds;
        c.region_copies = stats.region_copies;
    }
    c.layers_isolated = renderer_->layers_isolated();
    c.layers_flat = renderer_->layers_flat();
    c.glass_draws = renderer_->glass_draws();
    const rendering::gpu::OffscreenPoolStats pool = renderer_->offscreen_stats();
    c.offscreen_targets = pool.targets;
    c.offscreen_peak_live = pool.peak_live;
    c.offscreen_bytes = pool.bytes;
    c.offscreen_refused = pool.exhausted;
    c.damage_rects = plan.scissors.size();
    c.full_redraw = plan.full_redraw ? 1 : 0;
    driver_->end_frame();
    return result;
}

SceneProfile Fixture::profile_cold_frame(const char* name) {
    driver_->request_full_repaint();
    return profile_frame(name, false);
}

std::vector<SceneProfile> Fixture::profile() {
    std::vector<SceneProfile> scenes;
    driver_->request_full_repaint();
    scenes.push_back(profile_frame("cold_first_frame", false));

    edit_text();
    scenes.push_back(profile_frame("text_edit_full", false));

    edit_container();
    scenes.push_back(profile_frame("container_resize", false));

    edit_text("Card 0 summary title, edited again so exactly one node is damaged");
    scenes.push_back(profile_frame("text_edit_damaged", true));

    // Two repaints record then replay the retained draw lists (issue #476); the third is measured.
    for (int repeat = 0; repeat < 3; ++repeat) {
        driver_->request_full_repaint();
        SceneProfile frame = profile_frame("repeat_full_repaint", false);
        if (repeat == 2) {
            scenes.push_back(std::move(frame));
        }
    }

    // The first glass frame creates the pooled blur targets (a frame-boundary cost); the second is
    // the steady state.
    enable_glass_bar();
    for (int repeat = 0; repeat < 2; ++repeat) {
        driver_->request_full_repaint();
        SceneProfile frame = profile_frame("glass_full_paint", false);
        if (repeat == 1) {
            scenes.push_back(std::move(frame));
        }
    }
    return scenes;
}

} // namespace tessera::benchmark
