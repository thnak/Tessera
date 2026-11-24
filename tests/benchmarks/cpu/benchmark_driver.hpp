// SPDX-License-Identifier: MIT
//
// Frame driver for the CPU benchmark (issue #241). It is the headless frame loop of
// `src/app/host.cpp` (tick -> style -> layout -> scene-style sync -> damage -> paint) with one
// benchmark-only difference that makes the measured paint comparable to the paint trigger of
// docs/design/performance.md section 9.1: computed styles are resolved once into a table and reused
// by layout and paint instead of being re-derived from the cascade for every consumer.
//
// That split is the honest one for this benchmark: "style + layout" measures the CSS cascade plus
// Yoga, and "paint" measures draw-list building over already-computed styles. The production
// `AppHost` resolves styles lazily through `SceneStyleProvider`, so it pays the cascade again in
// layout and in paint; the numbers this benchmark reports would therefore be dominated by repeated
// CSS resolution, not by layout or paint. The driver keeps every other production step intact:
// DirtyPass, FramePipeline, DamageTracker, redraw plans, paint cache and the scene-style sync.

#pragma once

#include "tessera/rendering/damage.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/layout/apply_layout.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/layout/layout_scheduler.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cstddef>
#include <memory>
#include <unordered_map>

namespace tessera::benchmark {

// What one begin_frame() did, mirroring app::HostFrame.
struct FrameWork {
    ui::frame::FrameTick tick;
    ui::frame::FrameRender render;
    ui::layout::LayoutApplyResult layout;
    ui::paint::SceneStyleSyncResult sync;
    bool geometry = false;
};

// Wall time of each phase of the last begin_frame(), in milliseconds (the layout phases are zero
// when the frame did no layout). A breakdown, not a gate: it says where an incremental frame goes.
struct FramePhases {
    double tick_ms = 0.0;
    double styles_ms = 0.0;
    double layout_input_ms = 0.0;
    double layout_engine_ms = 0.0;
    double apply_layout_ms = 0.0;
    double sync_style_ms = 0.0;
    double finish_ms = 0.0;
};

class BenchmarkDriver final {
public:
    // `root` is borrowed and must outlive the driver. `viewport` is the logical content size.
    BenchmarkDriver(ui::Node& root, ui::style::CompiledStyleSheet sheet,
                    ui::layout::LayoutViewport viewport);
    ~BenchmarkDriver();

    BenchmarkDriver(const BenchmarkDriver&) = delete;
    BenchmarkDriver& operator=(const BenchmarkDriver&) = delete;

    // One-shot full-target repaint latch, like AppHost::request_full_repaint().
    void request_full_repaint();
    [[nodiscard]] FrameWork begin_frame();
    // Where the last begin_frame() spent its time.
    [[nodiscard]] const FramePhases& phases() const {
        return phases_;
    }
    [[nodiscard]] ui::frame::FramePaint paint(ui::paint::PaintSink& sink, bool force);
    void end_frame();

private:
    struct CachedStyles;

    [[nodiscard]] ui::style::ComputedStyle compute_one(const ui::Node& node,
                                                       const ui::style::ComputedStyle* parent);
    void resolve_all_styles();
    void resolve_subtree(ui::Node& node);
    void resolve_dirty_styles();
    void run_layout_pass();

    ui::Node* root_ = nullptr;
    ui::style::CompiledStyleSheet sheet_;
    ui::style::SceneStyleTree style_tree_;
    std::unordered_map<ui::NodeId, ui::style::ComputedStyle> styles_;
    std::unique_ptr<CachedStyles> provider_;
    ui::layout::LayoutInput layout_input_;
    runtime::ManualClock clock_;
    runtime::RuntimeLoop loop_;
    ui::animation::AnimationRuntime animations_;
    ui::DirtyPass dirty_;
    ui::layout::DamageTracker tracker_;
    rendering::DamageAccumulator damage_;
    ui::frame::FramePipeline pipeline_;
    ui::layout::LayoutEngine engine_;
    ui::layout::LayoutScheduler scheduler_;
    ui::layout::LayoutViewport viewport_;
    ui::layout::LayoutResult last_layout_;
    ui::layout::LayoutApplyResult last_applied_;
    ui::frame::FrameRender last_render_;
    FramePhases phases_;
    // Scoped-sync inputs, as the host keeps them: the nodes restyled this frame and the topmost
    // nodes whose box size changed. Members so a steady frame reuses their capacity.
    std::vector<const ui::Node*> sync_roots_;
    std::vector<const ui::Node*> resized_nodes_;
    bool styles_resolved_ = false;
    bool full_repaint_pending_ = false;
};

} // namespace tessera::benchmark
