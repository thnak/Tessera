// SPDX-License-Identifier: MIT
//
// One benchmark run: a fresh reference scene, the frame driver, and the null-RHI batched renderer
// that records the paint into draw lists. Every run rebuilds the fixture so the first frame is
// genuinely cold (cold selector tree, cold layout scheduler, cold paint cache). Rebuilds are not
// timed; only the style/layout/paint work between `measure()`'s timestamps is.

#pragma once

#include "benchmark_driver.hpp"
#include "benchmark_scene.hpp"
#include "benchmark_sink.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/image/image_decode_cache.hpp"
#include "tessera/rendering/image/image_texture_cache.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"

#include <cstddef>
#include <memory>

namespace tessera::benchmark {

struct PaintOutcome {
    double ms = 0.0;
    std::size_t draw_calls = 0;
    std::size_t quads = 0;
    // Quads the renderer dropped before staging them (issue #474); quads + quads_culled is what the
    // paint produced.
    std::size_t quads_culled = 0;
    std::size_t primitives = 0;
    std::size_t text_runs = 0;
    std::size_t images = 0;
    std::size_t paint_nodes = 0;
    // Paint-pass culling (issue #487): nodes whose own emission this paint skipped, and clipping
    // subtrees it skipped whole.
    std::size_t culled_nodes = 0;
    std::size_t culled_subtrees = 0;
    std::size_t backdrops = 0;
};

// One measured run. Times are per scenario; counters are from the full paint.
struct Measurement {
    double first_style_layout_ms = 0.0;
    double full_paint_ms = 0.0;
    double layout_paint_text_ms = 0.0;
    double layout_paint_container_ms = 0.0;
    // The phases of the one-text-change frame (the incremental frame this scene is measured for).
    FramePhases text_phases;
    // Glass scenario (issue #244): a full paint of the reference scene with the glass bar active.
    double glass_paint_ms = 0.0;
    std::size_t glass_backdrops = 0;
    // Damaged scenario (issue #474): paint only, after a one-node edit, through `begin_damaged`.
    // `damaged_partial` false means the plan fell back to a full redraw and the counters below
    // describe a full paint again.
    double damaged_paint_ms = 0.0;
    bool damaged_partial = false;
    // Nodes that resolved a style in that paint, and what the paint-pass cull (issue #487) skipped.
    // Deterministic: the same plan and scene produce the same counts on every run.
    std::size_t damaged_paint_nodes = 0;
    std::size_t damaged_culled_nodes = 0;
    std::size_t damaged_culled_subtrees = 0;
    std::size_t damaged_quads = 0;
    std::size_t damaged_quads_culled = 0;
    std::size_t damaged_draw_calls = 0;
    std::size_t damaged_damage_rects = 0;
    std::size_t damage_rects_full = 0;
    std::size_t damage_rects_text = 0;
    std::size_t damage_rects_container = 0;
    std::size_t draw_calls = 0;
    std::size_t quads = 0;
    std::size_t quads_culled = 0;
    std::size_t primitives = 0;
    std::size_t text_runs = 0;
    std::size_t images = 0;
    std::size_t paint_nodes = 0;
};

// Per-run maxima, accumulated by the caller across runs (the counters are deterministic, the times
// are summarized as median/p95).
struct Totals {
    // Filled by main from the collected samples.
    std::size_t damage_rects = 0;
    std::size_t draw_calls = 0;
    std::size_t quads = 0;
    std::size_t quads_culled = 0;
    std::size_t damaged_quads = 0;
    std::size_t damaged_quads_culled = 0;
    std::size_t damaged_draw_calls = 0;
    std::size_t damaged_damage_rects = 0;
    bool damaged_partial = false;
    std::size_t damaged_paint_nodes = 0;
    std::size_t damaged_culled_nodes = 0;
    std::size_t damaged_culled_subtrees = 0;
    std::size_t primitives = 0;
    std::size_t text_runs = 0;
    std::size_t images = 0;
    std::size_t paint_nodes = 0;
    std::size_t glass_backdrops = 0;
};

class Fixture final {
public:
    Fixture();
    ~Fixture();

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    [[nodiscard]] Measurement measure();
    [[nodiscard]] std::size_t nodes() const noexcept {
        return targets_.nodes;
    }
    // The scene root, laid out once `measure()` has run. The scene-walk benchmark reads it.
    [[nodiscard]] ui::Node& root() noexcept {
        return *root_;
    }

private:
    // `plan` selects the renderer entry point: null (or a full plan) is `begin()`, a partial plan
    // is `begin_damaged()`, so the culling sees the plan's damage bounding box instead of the
    // target.
    [[nodiscard]] PaintOutcome paint(bool force, const rendering::RedrawPlan* plan = nullptr);
    // `text == nullptr` uses the default rewrite; a different string is a different edit, which the
    // damaged scenario needs to produce a real (non-empty) damage plan.
    void edit_text(const char* text = nullptr);
    void edit_container();
    void enable_glass_bar();

    std::unique_ptr<ui::Node> root_;
    SceneBuilder builder_;
    SceneTargets targets_{};
    std::unique_ptr<rendering::rhi::Device> device_;
    std::unique_ptr<rendering::rhi::RenderTarget> target_;
    std::unique_ptr<rendering::rhi::Texture> white_;
    std::unique_ptr<rendering::rhi::Pipeline> pipeline_;
    // Both glass pipelines (issue #244) so the glass scenario records the real composite, including
    // the two blur passes and the sampled binds, exactly as the window's renderer does.
    std::unique_ptr<rendering::rhi::Pipeline> glass_blur_pipeline_;
    std::unique_ptr<rendering::rhi::Pipeline> glass_composite_pipeline_;
    std::unique_ptr<rendering::rhi::CommandList> commands_;
    std::unique_ptr<rendering::gpu::Batched2DRenderer> renderer_;
    ui::assets::AssetBundle bundle_;
    rendering::image::ImageDecodeCache decoded_;
    rendering::image::ImageTextureCache textures_;
    ui::paint::Batched2DSink batched_;
    CountingSink sink_;
    std::unique_ptr<BenchmarkDriver> driver_;
};

} // namespace tessera::benchmark
