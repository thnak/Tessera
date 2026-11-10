// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/loop_bridge.hpp"
#include "tessera/ui/animation/property_dirty.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace tessera::ui::frame {
namespace {

namespace rhi = tessera::rendering::rhi;

void index_nodes(Node& node, std::unordered_map<NodeId, Node*>& index) {
    index[node.id()] = &node;
    for (const auto& child : node.children()) {
        index_nodes(*child, index);
    }
}

rendering::RedrawPlan normalized_plan(rendering::RedrawPlan plan, rendering::rhi::Extent2D target,
                                      const rendering::RedrawPolicy& policy) {
    if (!rendering::should_full_redraw(plan, target, policy)) {
        return plan;
    }
    plan.full_redraw = true;
    plan.scissors.clear();
    if (target.width > 0 && target.height > 0) {
        plan.scissors.push_back(rendering::rhi::Scissor{0, 0, target.width, target.height});
    }
    return plan;
}

} // namespace

FramePipeline::FramePipeline(animation::AnimationRuntime& animations, runtime::RuntimeLoop& loop,
                             layout::DamageTracker& damage_tracker,
                             rendering::DamageAccumulator& damage, DirtyPass& dirty,
                             FramePipelineConfig config)
    : animations_(&animations),
      loop_(&loop),
      damage_tracker_(&damage_tracker),
      damage_(&damage),
      dirty_(&dirty),
      config_(config) {
    if (!std::isfinite(config_.dpi_scale) || config_.dpi_scale <= 0.0f) {
        throw std::invalid_argument("frame pipeline dpi scale must be finite and positive");
    }
    if (!std::isfinite(config_.damage_margin) || config_.damage_margin < 0.0f) {
        throw std::invalid_argument("frame pipeline damage margin must be finite and non-negative");
    }
}

FrameTick FramePipeline::tick(Node& root, double dt) {
    damage_->clear();
    // A new frame invalidates the previous frame's render decision; only finish_frame() can make a
    // frame due again.
    frame_due_ = false;

    FrameTick result;
    result.animation = animation::tick_animations(*animations_, *loop_, dt);

    if (!result.animation.samples.empty()) {
        std::unordered_map<NodeId, Node*> index;
        index_nodes(root, index);
        for (const animation::AnimationSample& sample : result.animation.samples) {
            const auto found = index.find(sample.target);
            if (found == index.end()) {
                continue;
            }
            const Dirty dirty = animation::dirty_for_declarations(sample.declarations);
            if (dirty != Dirty::Clean) {
                found->second->mark_dirty(dirty);
            }
        }
    }

    dirty_->collect(root, Dirty::All);
    for (const DirtyNode& entry : dirty_->nodes()) {
        result.needs_style = result.needs_style || has_flag(entry.flags, Dirty::Style);
        result.needs_layout = result.needs_layout || has_flag(entry.flags, Dirty::Layout);
    }
    if (dirty_->overflowed()) {
        result.needs_style = true;
        result.needs_layout = true;
    }
    return result;
}

paint::SceneStyleSyncResult FramePipeline::sync_style(Node& root, paint::StyleProvider& styles,
                                                      const FrameTick& tick, bool force) const {
    if (!force && !tick.needs_style && !tick.needs_layout) {
        return {};
    }
    return paint::sync_scene_style(root, styles);
}

FrameRender FramePipeline::finish_frame(Node& root) {
    FrameRender result;
    dirty_->collect(root, Dirty::Paint | Dirty::Render);
    result.damage = damage_tracker_->add_scene_damage(*dirty_, *damage_, config_.dpi_scale,
                                                      config_.damage_margin);
    result.redraw_plan = normalized_plan(rendering::make_redraw_plan(*damage_), damage_->target(),
                                         config_.redraw_policy);
    // The damage pass already latches a full redraw on a dirty-pass overflow; keep the decision
    // here as well so a future plan source cannot silently under-damage a truncated frame.
    if (result.damage.overflowed && !result.redraw_plan.full_redraw) {
        result.redraw_plan.full_redraw = true;
        result.redraw_plan.scissors.clear();
        const rendering::rhi::Extent2D target = damage_->target();
        if (target.width > 0 && target.height > 0) {
            result.redraw_plan.scissors.push_back(
                rendering::rhi::Scissor{0, 0, target.width, target.height});
        }
    }
    result.full_redraw = result.redraw_plan.full_redraw;
    result.should_render = !result.redraw_plan.scissors.empty();
    frame_due_ = result.should_render;
    return result;
}

FramePaint FramePipeline::paint_frame(const Node& root, paint::StyleProvider& styles,
                                      paint::PaintSink& sink, bool force) const {
    FramePaint result;
    if (!frame_due_ && !force) {
        return result;
    }
    result.stats = paint::paint_scene(root, styles, sink);
    result.painted = true;
    return result;
}

FramePaint FramePipeline::paint_frame(const Node& root, paint::StyleProvider& styles,
                                      paint::PaintSink& sink, const rendering::RedrawPlan& plan,
                                      bool force) const {
    FramePaint result;
    if (!frame_due_ && !force) {
        return result;
    }
    if (plan.full_redraw) {
        result.stats = paint::paint_scene(root, styles, sink);
        result.painted = true;
        return result;
    }
    if (plan.scissors.empty()) {
        return result;
    }
    paint::ClippedPaintSink clipped(sink, plan.scissors);
    result.stats = paint::paint_scene(root, styles, clipped);
    result.painted = true;
    return result;
}

} // namespace tessera::ui::frame
