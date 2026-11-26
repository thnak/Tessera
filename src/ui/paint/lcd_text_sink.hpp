// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

// LCD text eligibility at the paint seam (display-aware-rendering.md section 3.4, 3.4.1 step 3).
//
// Whether a run may composite per-channel is a property of the node it belongs to, not of the run:
// the window permission, the node's effective opacity and the transform it paints under. The text
// helpers build `TextDraw`s at seven call sites, so instead of threading a flag through every one,
// the node-level fact is applied once, by tagging the sink the helpers draw into.
namespace tessera::ui::paint::detail {

// True when text painted by a node may use LCD coverage. A run is fringe-free only when it lands on
// the device pixel grid it was rasterized for and blends over an opaque target:
//   - `context.lcd_text` is the window-level permission (opaque window, known horizontal stripe,
//     the policy's `LcdWhenEligible`), so a translucent window never reaches the rest;
//   - effective opacity must be exactly 1, so a faded group never carries colour fringes;
//   - the node paints under a translate-only matrix with no interpolation override: any scale,
//     rotation or skew resamples the glyph quad off the grid, and an animated node moves between
//     frames.
// Content beneath the run does not matter on an opaque target: dual-source blending is per-channel
// source-over, which is correct over any opaque pixel regardless of how it was painted.
[[nodiscard]] inline bool lcd_text_eligible(const PaintContext& context, float effective_opacity,
                                            bool frame_active, bool interpolated,
                                            const rhi::Mat3& painted_world) {
    const float* m = painted_world.m;
    const bool translate_only = m[0] == 1.0f && m[1] == 0.0f && m[3] == 0.0f && m[4] == 1.0f &&
                                m[6] == 0.0f && m[7] == 0.0f && m[8] == 1.0f;
    return context.lcd_text && effective_opacity >= 1.0f && !frame_active && !interpolated &&
           translate_only;
}

// Forwards every command to `target` and marks the text runs it forwards as LCD-eligible.
class LcdTextSink final : public PaintSink {
public:
    explicit LcdTextSink(PaintSink& target)
        : target_(&target) {}

    void fill_rect(const rhi::Rect& rect, const rhi::Color& color) override {
        target_->fill_rect(rect, color);
    }
    void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color, float radius) override {
        target_->fill_rounded_rect(rect, color, radius);
    }
    void fill_rounded_ring(const rhi::Rect& rect, const rhi::Color& color, float radius,
                           float width) override {
        target_->fill_rounded_ring(rect, color, radius, width);
    }
    void fill_linear_gradient(const GradientDraw& draw) override {
        target_->fill_linear_gradient(draw);
    }
    void push_clip(const RoundedClip& clip) override {
        target_->push_clip(clip);
    }
    void pop_clip() override {
        target_->pop_clip();
    }
    [[nodiscard]] bool supports_transform() const override {
        return target_->supports_transform();
    }
    void push_transform(const rhi::Mat3& transform) override {
        target_->push_transform(transform);
    }
    void pop_transform() override {
        target_->pop_transform();
    }
    void draw_text(const TextDraw& run) override {
        TextDraw tagged = run;
        tagged.lcd = true;
        target_->draw_text(tagged);
    }
    void draw_vector(const VectorDraw& draw) override {
        target_->draw_vector(draw);
    }
    void draw_image(const ImageDraw& draw) override {
        target_->draw_image(draw);
    }
    [[nodiscard]] bool supports_box_shadow() const override {
        return target_->supports_box_shadow();
    }
    void fill_box_shadow(const BoxShadowDraw& draw) override {
        target_->fill_box_shadow(draw);
    }
    [[nodiscard]] bool supports_backdrop() const override {
        return target_->supports_backdrop();
    }
    void draw_backdrop(const BackdropDraw& draw) override {
        target_->draw_backdrop(draw);
    }
    [[nodiscard]] bool supports_layers() const override {
        return target_->supports_layers();
    }
    void push_layer(const LayerDraw& layer) override {
        target_->push_layer(layer);
    }
    void pop_layer() override {
        target_->pop_layer();
    }

private:
    PaintSink* target_;
};

} // namespace tessera::ui::paint::detail
