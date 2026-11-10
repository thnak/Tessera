// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/color.hpp"

#include <array>
#include <map>
#include <optional>
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

using PaintSamples = std::map<NodeId, PaintSample>;

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

// The frame pipeline's paint seam: a read-only view over one frame's interpolated tick samples.
// The paint pass applies it to its local style copy and to the matrix it paints with, so a frame
// between ticks shows the blended visual state while Node::bounds()/transform(), damage and hit
// testing keep the authoritative tick geometry. Never mutates the scene. An empty view paints the
// authoritative state (the pass takes the untouched path), so callers can pass it unconditionally.
class InterpolationPaintView final : public paint::PaintView {
public:
    InterpolationPaintView() = default;
    explicit InterpolationPaintView(PaintSamples samples)
        : samples_(std::move(samples)) {}

    [[nodiscard]] bool empty() const {
        return samples_.empty();
    }
    [[nodiscard]] const PaintSamples& samples() const {
        return samples_;
    }

    [[nodiscard]] std::optional<rendering::rhi::Mat3> local_transform(NodeId node) const override;
    void overlay_style(NodeId node, style::ComputedStyle& style) const override;

private:
    PaintSamples samples_;
};

} // namespace tessera::ui::frame
