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
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

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
    // Resolved styles indexed by node id. The reference scene hands out ids 1..N, so the table is a
    // plain array and a lookup is an index, not a hash; an id past the end or never stored reads as
    // "no style". It never shrinks and is reserved before the first frame, so a reference stays
    // valid.
    class StyleTable {
    public:
        void reserve(std::size_t count) {
            slots_.reserve(count);
            present_.reserve(count);
        }
        void put(ui::NodeId id, const ui::style::ComputedStyle& style) {
            const auto index = static_cast<std::size_t>(id);
            if (index >= slots_.size()) {
                slots_.resize(index + 1);
                present_.resize(index + 1, 0);
            }
            slots_[index] = style;
            present_[index] = 1;
        }
        [[nodiscard]] const ui::style::ComputedStyle* get(ui::NodeId id) const {
            const auto index = static_cast<std::size_t>(id);
            return index < slots_.size() && present_[index] != 0 ? &slots_[index] : nullptr;
        }

    private:
        std::vector<ui::style::ComputedStyle> slots_;
        std::vector<std::uint8_t> present_;
    };
    StyleTable styles_;
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
    // The retained layout input may be behind the scene (never built, or a frame left dirt it did
    // not see); the next pass then rebuilds every node, as the host does (layout_input_reuse.hpp).
    bool input_stale_ = true;
    bool input_built_this_frame_ = false;
    bool full_repaint_pending_ = false;
};

} // namespace tessera::benchmark
