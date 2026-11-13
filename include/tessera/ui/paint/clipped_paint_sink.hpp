// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_sink.hpp"

#include <algorithm>
#include <vector>

namespace tessera::ui::paint {

namespace rhi = tessera::rendering::rhi;

namespace detail {

inline rhi::Rect region_rect(const rhi::Scissor& scissor) {
    return rhi::Rect{static_cast<float>(scissor.x), static_cast<float>(scissor.y),
                     static_cast<float>(scissor.width), static_cast<float>(scissor.height)};
}

inline rhi::Rect clipped_rect(const rhi::Rect& rect, const rhi::Rect& region) {
    const float x0 = std::max(rect.x, region.x);
    const float y0 = std::max(rect.y, region.y);
    const float x1 = std::min(rect.right(), region.right());
    const float y1 = std::min(rect.bottom(), region.bottom());
    if (x1 <= x0 || y1 <= y0) {
        return rhi::Rect{};
    }
    return rhi::Rect{x0, y0, x1 - x0, y1 - y0};
}

inline bool covers(const rhi::Rect& region, const rhi::Rect& rect) {
    return rect.x >= region.x && rect.y >= region.y && rect.right() <= region.right() &&
           rect.bottom() <= region.bottom();
}

} // namespace detail

// Damage-clipped replay sink. Mirrors Batched2DRenderer::begin_damaged at the paint level: every
// command is clipped to each redraw region, in region order. Regions from make_redraw_plan are
// disjoint (the damage accumulator AABB-merges touching rects), so no pixel is emitted twice; an
// overlapping caller plan deterministically emits the overlap once per covering region.
//
// Text and vector runs cannot be trimmed like a rect: they are replayed once per intersecting
// damage region under a square clip for that region, with their origin unchanged, so glyph/asset
// positions match a full paint while pixels outside every region are not repainted.
class ClippedPaintSink final : public PaintSink {
public:
    ClippedPaintSink(PaintSink& target, const std::vector<rhi::Scissor>& regions)
        : target_(&target),
          regions_(&regions) {}

    void fill_rect(const rhi::Rect& rect, const rhi::Color& color) override {
        if (transformed()) {
            replay([this, &rect, &color] { target_->fill_rect(rect, color); }, device_bounds(rect));
            return;
        }
        for (const rhi::Scissor& scissor : *regions_) {
            const rhi::Rect clipped = detail::clipped_rect(rect, detail::region_rect(scissor));
            if (clipped.width > 0.0f && clipped.height > 0.0f) {
                target_->fill_rect(clipped, color);
            }
        }
    }

    void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color, float radius) override {
        if (transformed()) {
            replay(
                [this, &rect, &color, radius] { target_->fill_rounded_rect(rect, color, radius); },
                device_bounds(rect));
            return;
        }
        for (const rhi::Scissor& scissor : *regions_) {
            const rhi::Rect region = detail::region_rect(scissor);
            if (detail::covers(region, rect)) {
                target_->fill_rounded_rect(rect, color, radius);
                continue;
            }
            const rhi::Rect clipped = detail::clipped_rect(rect, region);
            if (clipped.width > 0.0f && clipped.height > 0.0f) {
                // A rounded rect cut by an axis-aligned region has no PaintSink representation;
                // repaint the intersection as a plain rect so nothing inside the region is missed.
                target_->fill_rect(clipped, color);
            }
        }
    }

    // Clips are orthogonal to damage: forward them unchanged so the target sink applies the same
    // rounded-clip stack the full paint would have used.
    void push_clip(const RoundedClip& clip) override {
        target_->push_clip(clip);
    }
    void pop_clip() override {
        target_->pop_clip();
    }

    void fill_linear_gradient(const GradientDraw& draw) override {
        replay([this, &draw] { target_->fill_linear_gradient(draw); }, device_bounds(draw.rect));
    }

    void draw_text(const TextDraw& run) override {
        replay([this, &run] { target_->draw_text(run); }, device_bounds(run.rect));
    }

    void draw_vector(const VectorDraw& draw) override {
        replay([this, &draw] { target_->draw_vector(draw); }, device_bounds(draw.rect));
    }

    void draw_image(const ImageDraw& draw) override {
        replay([this, &draw] { target_->draw_image(draw); }, device_bounds(draw.rect));
    }

    // A glass backdrop is replayed once per intersecting damage region under that region's clip,
    // like the other segment-like draws: the blurred result is recomputed for the painted regions
    // only, and pixels outside every region are not repainted.
    void draw_backdrop(const BackdropDraw& draw) override {
        replay([this, &draw] { target_->draw_backdrop(draw); }, device_bounds(draw.rect));
    }

    [[nodiscard]] bool supports_backdrop() const override {
        return target_->supports_backdrop();
    }

    // Matrices pass through to the target; damage regions stay in device space. Commands issued
    // under a matrix are replayed per region under a device-space clip (like text), because their
    // rects are frame-local and cannot be trimmed arithmetically. Clips arriving from the paint
    // pass are already device space and are forwarded unchanged.
    [[nodiscard]] bool supports_transform() const override {
        return target_->supports_transform();
    }
    void push_transform(const rhi::Mat3& transform) override {
        composed_.push_back(composed_.empty() ? transform : composed_.back() * transform);
        target_->push_transform(transform);
    }
    void pop_transform() override {
        if (!composed_.empty()) {
            composed_.pop_back();
        }
        target_->pop_transform();
    }

private:
    [[nodiscard]] bool transformed() const {
        return !composed_.empty();
    }

    // Device-space bounding box of a frame-local rect under the active matrix.
    [[nodiscard]] rhi::Rect device_bounds(const rhi::Rect& rect) const {
        if (composed_.empty()) {
            return rect;
        }
        const rhi::Mat3& m = composed_.back();
        float xs[4];
        float ys[4];
        m.transform_point(rect.x, rect.y, xs[0], ys[0]);
        m.transform_point(rect.right(), rect.y, xs[1], ys[1]);
        m.transform_point(rect.right(), rect.bottom(), xs[2], ys[2]);
        m.transform_point(rect.x, rect.bottom(), xs[3], ys[3]);
        const auto [min_x, max_x] = std::minmax_element(xs, xs + 4);
        const auto [min_y, max_y] = std::minmax_element(ys, ys + 4);
        return rhi::Rect{*min_x, *min_y, *max_x - *min_x, *max_y - *min_y};
    }

    template<typename Draw> void replay(Draw&& draw, const rhi::Rect& bounds) {
        for (const rhi::Scissor& scissor : *regions_) {
            const rhi::Rect region = detail::region_rect(scissor);
            const rhi::Rect overlap = detail::clipped_rect(bounds, region);
            if (overlap.width <= 0.0f || overlap.height <= 0.0f) {
                continue;
            }
            RoundedClip clip;
            clip.rect = region;
            target_->push_clip(clip);
            draw();
            target_->pop_clip();
        }
    }

    PaintSink* target_ = nullptr;
    const std::vector<rhi::Scissor>* regions_ = nullptr;
    std::vector<rhi::Mat3> composed_;
};

} // namespace tessera::ui::paint
