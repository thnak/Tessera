// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node_fwd.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/color.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

// Paint-only interpolation between two simulation ticks (docs/design/game-loop.md section 3.1).
//
// The fixed-step loop reports an `alpha` fraction of a step elapsed since the last tick
// (runtime::interpolation_alpha). A render that happens between ticks blends the *previous* and the
// *current* tick sample with that alpha, so motion is smooth at render rate while simulation stays
// deterministic.
//
// Only paint-time visual state is sampled: transform, opacity and colors. Layout boxes, scroll
// offsets that feed layout, and text advances are never part of a sample, so no render-rate pass
// can move a box or change what hit testing sees. The scene graph stays authoritative for geometry.
namespace tessera::ui::frame {

// One node's paint-only visual state at one simulation tick. Absent fields keep whatever the
// caller's style provider resolved for that node.
struct PaintSample {
    // Local transform around the node's box origin, in the shape Node::set_transform receives.
    std::optional<rendering::rhi::Mat3> transform;
    std::optional<float> opacity; // clamped 0..1
    std::optional<style::ColorRgba> color;
    std::optional<style::ColorRgba> background_color;
    // Border colors in paint order: top, right, bottom, left.
    std::array<std::optional<style::ColorRgba>, 4> border_colors;
};

// A sorted flat map of node id -> sample. std::map allocated a node per entry every tick (and a
// sentinel on MSVC); this keeps its storage across ticks, so a steady animation allocates nothing
// for samples once the vector has grown (issue #701). Same interface the pipeline and tests use.
class PaintSamples {
public:
    using value_type = std::pair<NodeId, PaintSample>;
    using Storage = std::vector<value_type>;
    using iterator = Storage::iterator;
    using const_iterator = Storage::const_iterator;

    [[nodiscard]] iterator begin() {
        return items_.begin();
    }
    [[nodiscard]] iterator end() {
        return items_.end();
    }
    [[nodiscard]] const_iterator begin() const {
        return items_.begin();
    }
    [[nodiscard]] const_iterator end() const {
        return items_.end();
    }
    [[nodiscard]] bool empty() const {
        return items_.empty();
    }
    [[nodiscard]] std::size_t size() const {
        return items_.size();
    }
    void clear() {
        items_.clear();
    }
    void swap(PaintSamples& other) noexcept {
        items_.swap(other.items_);
    }

    [[nodiscard]] iterator find(NodeId node) {
        const iterator at = lower(node);
        return at != items_.end() && at->first == node ? at : items_.end();
    }
    [[nodiscard]] const_iterator find(NodeId node) const {
        const const_iterator at = lower(node);
        return at != items_.end() && at->first == node ? at : items_.end();
    }
    [[nodiscard]] bool contains(NodeId node) const {
        return find(node) != items_.end();
    }
    void erase(NodeId node) {
        if (const iterator at = find(node); at != items_.end()) {
            items_.erase(at);
        }
    }
    [[nodiscard]] const PaintSample& at(NodeId node) const {
        const const_iterator found = find(node);
        if (found == items_.end()) {
            throw std::out_of_range("PaintSamples::at");
        }
        return found->second;
    }
    // Inserts a default sample when absent. Ascending keys, the common case, append in O(1).
    PaintSample& operator[](NodeId node) {
        if (items_.empty() || items_.back().first < node) {
            items_.emplace_back(node, PaintSample{});
            return items_.back().second;
        }
        const iterator at = lower(node);
        if (at != items_.end() && at->first == node) {
            return at->second;
        }
        return items_.emplace(at, node, PaintSample{})->second;
    }

private:
    [[nodiscard]] iterator lower(NodeId node) {
        return std::lower_bound(
            items_.begin(), items_.end(), node,
            [](const value_type& item, NodeId key) { return item.first < key; });
    }
    [[nodiscard]] const_iterator lower(NodeId node) const {
        return std::lower_bound(
            items_.begin(), items_.end(), node,
            [](const value_type& item, NodeId key) { return item.first < key; });
    }

    Storage items_;
};

// Adds the paint-visible declarations of one animation sample to `scene_node`'s entry. A
// `transform` value is resolved against the node's current box (percentages and the default
// `transform-origin`); malformed values are ignored. Layout-feeding and text properties are never
// captured: a `width` or `font-size` animation stays discrete per tick (its owner resolves it
// through style/layout, not through this sample).
void capture_paint_sample(PaintSamples& samples, const Node& scene_node,
                          const std::vector<style::Declaration>& declarations);

// Blends two consecutive tick samples for a render that is `alpha` of the way from `previous` to
// `current` (alpha is expected in [0, 1]; anything else is clamped). The result has exactly the
// keys of `current`, because the current tick owns which properties are overridden: a property
// sampled on an earlier tick but no longer animated is dropped, not held. A key that is new in
// `current` keeps its current value (there is nothing to blend from), so alpha 0 reproduces
// `previous` for continuing values and `current` for first-tick values.
//
// Transforms blend as translate/scale/rotate components (rotation takes the shorter arc); a matrix
// that cannot be expressed that way (a skew or `matrix()` with a skew) holds the current value.
// Colors blend in sRGB, like the animation sampler.
[[nodiscard]] PaintSamples interpolate_paint_samples(const PaintSamples& previous,
                                                     const PaintSamples& current, double alpha);
// Same blend written into `out`, whose storage is reused (the pipeline keeps one per frame).
void interpolate_paint_samples_into(PaintSamples& out, const PaintSamples& previous,
                                    const PaintSamples& current, double alpha);

// The frame pipeline's paint seam: a read-only view over one frame's interpolated tick samples.
// The paint pass applies it to its local style copy and to the matrix it paints with, so a frame
// between ticks shows the blended visual state while Node::bounds()/transform(), damage and hit
// testing keep the authoritative tick geometry. Never mutates the scene. An empty view paints the
// authoritative state (the pass takes the untouched path), so callers can pass it unconditionally.
class InterpolationPaintView final : public paint::PaintView {
public:
    InterpolationPaintView() = default;
    // Owns `samples`. An empty set makes an empty view (the pass takes the untouched path).
    explicit InterpolationPaintView(PaintSamples samples)
        : owned_(std::move(samples)) {
        view_ = owned_.empty() ? nullptr : &owned_;
    }
    // Borrows samples the caller keeps alive (the pipeline caches one blended set per tick, so
    // building a view per paint allocates nothing, issue #701). It must not outlive them.
    explicit InterpolationPaintView(const PaintSamples* borrowed)
        : view_(borrowed != nullptr && !borrowed->empty() ? borrowed : nullptr) {}

    InterpolationPaintView(const InterpolationPaintView& other)
        : owned_(other.owned_),
          view_(other.view_ == &other.owned_ ? &owned_ : other.view_) {}
    InterpolationPaintView(InterpolationPaintView&& other) noexcept
        : owned_(std::move(other.owned_)),
          view_(other.view_ == &other.owned_ ? &owned_ : other.view_) {}
    InterpolationPaintView& operator=(const InterpolationPaintView&) = delete;
    InterpolationPaintView& operator=(InterpolationPaintView&&) = delete;
    ~InterpolationPaintView() override = default;

    [[nodiscard]] bool empty() const {
        return view_ == nullptr;
    }
    [[nodiscard]] const PaintSamples& samples() const {
        static const PaintSamples kNone;
        return view_ != nullptr ? *view_ : kNone;
    }

    [[nodiscard]] std::optional<rendering::rhi::Mat3> local_transform(NodeId node) const override;
    void overlay_style(NodeId node, style::ComputedStyle& style) const override;
    [[nodiscard]] bool overlays(NodeId node) const override {
        return view_ != nullptr && view_->contains(node);
    }

private:
    PaintSamples owned_;
    const PaintSamples* view_ = nullptr;
};

} // namespace tessera::ui::frame
