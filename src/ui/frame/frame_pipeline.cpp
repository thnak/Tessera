// SPDX-License-Identifier: MIT

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/loop_bridge.hpp"
#include "tessera/ui/animation/property_dirty.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/clipped_paint_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
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

// The axis-aligned box a matrix maps `rect` onto (the same rule Node::world_bounds uses).
rhi::Rect transformed_aabb(const rhi::Mat3& matrix, const rhi::Rect& rect) {
    const float xs[4] = {rect.x, rect.right(), rect.right(), rect.x};
    const float ys[4] = {rect.y, rect.y, rect.bottom(), rect.bottom()};
    float min_x = std::numeric_limits<float>::max();
    float min_y = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float max_y = std::numeric_limits<float>::lowest();
    for (int index = 0; index < 4; ++index) {
        float x = 0.0f;
        float y = 0.0f;
        matrix.transform_point(xs[index], ys[index], x, y);
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }
    return rhi::Rect{min_x, min_y, max_x - min_x, max_y - min_y};
}

// Device-pixel corners of `rect` under `matrix`, in a fixed order, for distance comparison.
std::array<float, 8> device_corners(const rhi::Mat3& matrix, const rhi::Rect& rect, float scale) {
    const float xs[4] = {rect.x, rect.right(), rect.right(), rect.x};
    const float ys[4] = {rect.y, rect.y, rect.bottom(), rect.bottom()};
    std::array<float, 8> corners{};
    for (std::size_t index = 0; index < 4; ++index) {
        matrix.transform_point(xs[index], ys[index], corners[index * 2], corners[index * 2 + 1]);
        corners[index * 2] *= scale;
        corners[index * 2 + 1] *= scale;
    }
    return corners;
}

bool moved_less_than(const std::array<float, 8>& a, const std::array<float, 8>& b, float limit) {
    for (std::size_t index = 0; index < 8; ++index) {
        if (std::abs(a[index] - b[index]) >= limit) {
            return false;
        }
    }
    return true;
}

// The matrix `node` paints with under `view`, composed exactly like the paint pass composes it:
// painted_world(n) = painted_world(parent) * translation(-parent_scroll) *
//                    translation(bounds.xy) * local(n). Kept here so damage can cover the
//                    interpolated box; paint_pass.cpp composes the same formula incrementally.
rhi::Mat3 painted_world(const Node& node, const paint::PaintView& view) {
    const std::optional<rhi::Mat3> own = view.local_transform(node.id());
    rhi::Mat3 world =
        rhi::Mat3::translation(node.bounds().x, node.bounds().y) * own.value_or(node.transform());
    bool overridden = own.has_value();
    for (const Node* parent = node.parent(); parent != nullptr; parent = parent->parent()) {
        const std::optional<rhi::Mat3> local = view.local_transform(parent->id());
        overridden = overridden || local.has_value();
        world = rhi::Mat3::translation(parent->bounds().x, parent->bounds().y) *
                local.value_or(parent->transform()) *
                rhi::Mat3::translation(-parent->scroll_x(), -parent->scroll_y()) * world;
    }
    return overridden ? world : node.world_transform();
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
    if (!std::isfinite(config_.interpolation_epsilon_px) ||
        config_.interpolation_epsilon_px < 0.0f) {
        throw std::invalid_argument(
            "frame pipeline interpolation epsilon must be finite and non-negative");
    }
}

void FramePipeline::set_update_scheduler(UpdateScheduler* scheduler) {
    if (scheduler_ != nullptr) {
        scheduler_->set_wake(nullptr, nullptr);
    }
    scheduler_ = scheduler;
    if (scheduler_ != nullptr) {
        scheduler_->set_wake(
            [](void* context) { static_cast<runtime::RuntimeLoop*>(context)->request_frame(); },
            loop_);
    }
}

FrameTick FramePipeline::tick(Node& root, double dt) {
    return tick_impl(root, dt, 0.0, false);
}

FrameTick FramePipeline::tick(Node& root, double dt, double alpha) {
    return tick_impl(root, dt, alpha, true);
}

FrameTick FramePipeline::tick_impl(Node& root, double dt, double alpha, bool interpolate) {
    alpha_ = std::isfinite(alpha) ? std::clamp(alpha, 0.0, 1.0) : 0.0;
    interpolate_ = interpolate && config_.interpolation;
    damage_->clear();
    // A new frame invalidates the previous frame's render decision; only finish_frame() can make a
    // frame due again.
    frame_due_ = false;

    // Scheduled component updates land before animation, style and layout, so a state change made
    // since the last tick is part of this frame (an effect or animation they start ticks now).
    if (scheduler_ != nullptr) {
        (void)scheduler_->tick(dt);
        (void)scheduler_->flush();
    }

    FrameTick result;
    result.animation = animation::tick_animations(*animations_, *loop_, dt);

    std::unordered_map<NodeId, Node*> index;
    tick_springs(root, dt, result.animation, index);
    if (!result.animation.samples.empty()) {
        if (index.empty()) {
            index_nodes(root, index);
        }
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

    // Rotate the paint-only tick samples. Only declarations paint reads are captured (transform,
    // opacity, colors); layout and text properties stay discrete in the style/layout path, so this
    // never requests layout work and never moves a box.
    previous_samples_ = std::move(current_samples_);
    current_samples_.clear();
    if (interpolate_) {
        for (const animation::AnimationSample& sample : result.animation.samples) {
            const auto found = index.find(sample.target);
            if (found == index.end()) {
                continue;
            }
            capture_paint_sample(current_samples_, *found->second, sample.declarations);
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

    // Completion events go out after this frame's state is settled and outside the animation tick,
    // so a listener may start new animations; wake the loop so they get their first tick.
    if (animations_->dispatch_events() > 0 && !animations_->empty()) {
        loop_->request_frame();
    }
    return result;
}

void FramePipeline::tick_springs(Node& root, double dt, animation::AnimationTickResult& into,
                                 std::unordered_map<NodeId, Node*>& index) {
    springs_.set_reduced_motion(animations_->reduced_motion());
    if (springs_.empty()) {
        return;
    }
    index_nodes(root, index);
    for (const std::uint64_t target : springs_.targets()) {
        if (index.find(target) == index.end()) {
            springs_.stop_target(target);
        }
    }
    animation::AnimationTickResult sprung = springs_.tick(dt);
    for (animation::AnimationSample& sample : sprung.samples) {
        into.samples.push_back(std::move(sample));
    }
    into.finished.insert(into.finished.end(), sprung.finished.begin(), sprung.finished.end());
    into.dirty = into.dirty | sprung.dirty;
    into.keep_awake = into.keep_awake || sprung.keep_awake;
    if (sprung.keep_awake || !sprung.finished.empty()) {
        loop_->request_frame();
    }
}

InterpolationPaintView FramePipeline::interpolation_view() const {
    if (!interpolate_ || current_samples_.empty()) {
        return {};
    }
    return InterpolationPaintView(
        interpolate_paint_samples(previous_samples_, current_samples_, alpha_));
}

void FramePipeline::invalidate_paint_cache() {
    // A style/tree generation change can restyle nodes whose own dirty flags stay Clean (inherited
    // values, ancestor/sibling selectors, a restyled root, a rebuilt selector tree). The cache key
    // deliberately cannot see a computed style, so the whole generation goes; re-recording is the
    // only way to guarantee the next frame is byte-identical to an uncached paint.
    paint_cache_.invalidate();
}

const paint::PaintCacheStats& FramePipeline::paint_cache_stats() const {
    paint_cache_stats_snapshot_ = paint_cache_.stats();
    return paint_cache_stats_snapshot_;
}

paint::SceneStyleSyncResult FramePipeline::sync_style(Node& root, paint::StyleProvider& styles,
                                                      const FrameTick& tick, bool force) const {
    if (!force && !tick.needs_style && !tick.needs_layout) {
        return {};
    }
    return paint::sync_scene_style(root, styles, config_.glass);
}

FrameRender FramePipeline::finish_frame(Node& root) {
    FrameRender result;
    dirty_->collect(root, Dirty::Paint | Dirty::Render);
    result.damage = damage_tracker_->add_scene_damage(*dirty_, *damage_, config_.dpi_scale,
                                                      config_.damage_margin);
    // Damage above is derived from the authoritative tick bounds. An interpolated frame paints
    // between the two ticks, which can fall outside both of them (a box moving more than its own
    // width per tick, a rotation whose intermediate AABB is wider than either endpoint). Add each
    // transform-sampled node's painted box so a partial redraw cannot leave stale pixels behind.
    const InterpolationPaintView view = interpolation_view();
    if (view.empty()) {
        last_damaged_corners_.clear();
    } else {
        std::unordered_map<NodeId, Node*> index;
        index_nodes(root, index);
        for (const auto& [node_id, sample] : view.samples()) {
            if (!sample.transform) {
                continue;
            }
            const auto found = index.find(node_id);
            if (found == index.end()) {
                continue;
            }
            const Node& node = *found->second;
            if (!node.visible()) {
                continue;
            }
            const rhi::Mat3 world = painted_world(node, view);
            const rhi::Rect local_box{0.0f, 0.0f, node.bounds().width, node.bounds().height};
            const std::array<float, 8> corners =
                device_corners(world, local_box, config_.dpi_scale);
            const auto last = last_damaged_corners_.find(node_id);
            if (last != last_damaged_corners_.end() && config_.interpolation_epsilon_px > 0.0f &&
                moved_less_than(corners, last->second, config_.interpolation_epsilon_px)) {
                ++result.interpolation_suppressed;
                continue;
            }
            last_damaged_corners_[node_id] = corners;
            rhi::Rect rect = transformed_aabb(
                world, rhi::Rect{0.0f, 0.0f, node.bounds().width, node.bounds().height});
            // Box-shadow reach is box-local: scale it like node_damage_rect does.
            const Node::PaintOutsets& outsets = node.paint_outsets();
            const float scale = std::max(
                {std::hypot(world.m[0], world.m[3]), std::hypot(world.m[1], world.m[4]), 0.0f});
            rect.x -= outsets.left * scale;
            rect.y -= outsets.top * scale;
            rect.width += (outsets.left + outsets.right) * scale;
            rect.height += (outsets.top + outsets.bottom) * scale;
            if (rect.width > 0.0f && rect.height > 0.0f) {
                damage_->add(rect, config_.damage_margin);
            }
        }
        // A node that stops being transform-sampled is painted from its tick transform from now on,
        // so its old interpolated position is no longer what is on screen.
        std::erase_if(last_damaged_corners_, [&view](const auto& entry) {
            const auto sample = view.samples().find(entry.first);
            return sample == view.samples().end() || !sample->second.transform;
        });
        result.damage.rects = damage_->rects().size();
        result.damage.full_redraw = damage_->full_redraw();
    }
    // Glass last: every damage source of the frame (dirty nodes, interpolated boxes) is in the
    // accumulator by now, so a glass node whose 3 sigma dependency region intersects any of it gets
    // its full box repainted (glass-v1 section 7). A tree without a glass node returns immediately.
    result.glass_dependencies = damage_tracker_->add_glass_dependencies(
        root, *damage_, config_.dpi_scale, config_.damage_margin);
    if (result.glass_dependencies > 0) {
        result.damage.rects = damage_->rects().size();
        result.damage.full_redraw = damage_->full_redraw();
    }
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
    const InterpolationPaintView view = interpolation_view();
    paint::PaintContext context;
    context.reduce_effects = reduce_effects_;
    context.high_contrast = high_contrast_;
    context.glass = config_.glass;
    if (config_.paint_cache) {
        context.paint_cache = &paint_cache_;
    }
    if (!view.empty()) {
        context.view = &view;
    }
    result.stats = paint::paint_scene(root, styles, sink, context);
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
    const InterpolationPaintView view = interpolation_view();
    paint::PaintContext context;
    context.reduce_effects = reduce_effects_;
    context.high_contrast = high_contrast_;
    context.glass = config_.glass;
    if (config_.paint_cache) {
        context.paint_cache = &paint_cache_;
    }
    if (!view.empty()) {
        context.view = &view;
    }
    if (plan.full_redraw) {
        result.stats = paint::paint_scene(root, styles, sink, context);
        result.painted = true;
        return result;
    }
    if (plan.scissors.empty()) {
        return result;
    }
    paint::ClippedPaintSink clipped(sink, plan.scissors);
    result.stats = paint::paint_scene(root, styles, clipped, context);
    result.painted = true;
    return result;
}

} // namespace tessera::ui::frame
