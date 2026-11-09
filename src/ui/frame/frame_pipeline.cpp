// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/loop_bridge.hpp"
#include "tessera/ui/animation/property_dirty.hpp"
#include "tessera/ui/frame_pipeline.hpp"
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

rhi::Rect region_rect(const rhi::Scissor& scissor) {
    return rhi::Rect{static_cast<float>(scissor.x), static_cast<float>(scissor.y),
                     static_cast<float>(scissor.width), static_cast<float>(scissor.height)};
}

rhi::Rect clip_rect(const rhi::Rect& rect, const rhi::Rect& region) {
    const float x0 = std::max(rect.x, region.x);
    const float y0 = std::max(rect.y, region.y);
    const float x1 = std::min(rect.right(), region.right());
    const float y1 = std::min(rect.bottom(), region.bottom());
    if (x1 <= x0 || y1 <= y0) {
        return rhi::Rect{};
    }
    return rhi::Rect{x0, y0, x1 - x0, y1 - y0};
}

bool covers(const rhi::Rect& region, const rhi::Rect& rect) {
    return rect.x >= region.x && rect.y >= region.y && rect.right() <= region.right() &&
           rect.bottom() <= region.bottom();
}

// Mirrors Batched2DRenderer::begin_damaged at the paint level: every command is clipped to each
// redraw region, in region order. Regions from make_redraw_plan are disjoint (the damage
// accumulator AABB-merges touching rects), so no pixel is emitted twice; an overlapping caller plan
// deterministically emits the overlap once per covering region.
class ClippedPaintSink final : public paint::PaintSink {
public:
    ClippedPaintSink(paint::PaintSink& target, const std::vector<rhi::Scissor>& regions)
        : target_(&target),
          regions_(&regions) {}

    void fill_rect(const rhi::Rect& rect, const rhi::Color& color) override {
        for (const rhi::Scissor& scissor : *regions_) {
            const rhi::Rect clipped = clip_rect(rect, region_rect(scissor));
            if (clipped.width > 0.0f && clipped.height > 0.0f) {
                target_->fill_rect(clipped, color);
            }
        }
    }

    void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color, float radius) override {
        for (const rhi::Scissor& scissor : *regions_) {
            const rhi::Rect region = region_rect(scissor);
            if (covers(region, rect)) {
                target_->fill_rounded_rect(rect, color, radius);
                continue;
            }
            const rhi::Rect clipped = clip_rect(rect, region);
            if (clipped.width > 0.0f && clipped.height > 0.0f) {
                // A rounded rect cut by an axis-aligned region has no PaintSink representation;
                // repaint the intersection as a plain rect so nothing inside the region is missed.
                target_->fill_rect(clipped, color);
            }
        }
    }

    // Clips are orthogonal to damage: forward them unchanged so the target sink applies the same
    // rounded-clip stack the full paint would have used.
    void push_clip(const paint::RoundedClip& clip) override {
        target_->push_clip(clip);
    }
    void pop_clip() override {
        target_->pop_clip();
    }

    // A text run cannot be trimmed like a rect: replay it once per intersecting damage region under
    // a square clip for that region. The run origin is unchanged, so glyph positions match a full
    // paint; pixels outside every damage region are deliberately not repainted.
    void draw_text(const paint::TextDraw& run) override {
        for (const rhi::Scissor& scissor : *regions_) {
            const rhi::Rect region = region_rect(scissor);
            const rhi::Rect overlap = clip_rect(run.rect, region);
            if (overlap.width <= 0.0f || overlap.height <= 0.0f) {
                continue;
            }
            paint::RoundedClip clip;
            clip.rect = region;
            target_->push_clip(clip);
            target_->draw_text(run);
            target_->pop_clip();
        }
    }

private:
    paint::PaintSink* target_ = nullptr;
    const std::vector<rhi::Scissor>* regions_ = nullptr;
};

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
            if (dirty != Dirty::None) {
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

FrameRender FramePipeline::finish_frame(Node& root) {
    FrameRender result;
    dirty_->collect(root, Dirty::Paint | Dirty::Render);
    result.damage = damage_tracker_->add_scene_damage(*dirty_, *damage_, config_.dpi_scale,
                                                      config_.damage_margin);
    result.redraw_plan = normalized_plan(rendering::make_redraw_plan(*damage_), damage_->target(),
                                         config_.redraw_policy);
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
    ClippedPaintSink clipped(sink, plan.scissors);
    result.stats = paint::paint_scene(root, styles, clipped);
    result.painted = true;
    return result;
}

} // namespace tessera::ui::frame
