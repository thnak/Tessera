// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <array>
#include <cstddef>
#include <vector>

// CSS `text-shadow` parsing and emission (issue #616). Internal to the paint library. Not
// installed.
//
// A text shadow is painted *behind* the glyphs of every run of a text node, including the runs of
// its inline spans, and it never changes layout or the glyph ink box: the shadow is emitted as
// ordinary `TextDraw` runs offset from the real ones, so the culling and fit code keeps reading the
// unshadowed ink box (docs/design/text-shadow.md section 3).
namespace tessera::ui::paint::detail {

namespace rhi = tessera::rendering::rhi;

// At most this many shadows are honored per node; extra ones are ignored (bounded, no allocation).
// CSS allows an unbounded list; V1 caps it like `box-shadow` does.
inline constexpr std::size_t kMaxTextShadows = 4;

// Rings of offset copies used to approximate a blurred shadow, and the copies placed on each ring.
// The product bounds the extra draws one blurred shadow costs per run: 4 * 8 = 32, plus the centred
// copy. `kReducedTextShadowRings` is the adaptive-quality cap (performance.md section 6.1).
inline constexpr int kMinTextShadowRings = 2;
inline constexpr int kMaxTextShadowRings = 4;
inline constexpr int kTextShadowRingSamples = 8;
inline constexpr int kReducedTextShadowRings = 1;

struct TextShadow {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float blur = 0.0f; // CSS blur radius in px (Gaussian sigma = blur / 2)
    rhi::Color color;
};

struct TextShadowList {
    std::array<TextShadow, kMaxTextShadows> items;
    std::size_t count = 0;

    [[nodiscard]] bool empty() const {
        return count == 0;
    }
};

// Parses the comma-separated `text-shadow` list: `[<color>? <x> <y> <blur>?]`, colour in either
// position, lengths in px (or a bare 0). A missing colour is `currentColor`. There is no spread and
// no `inset` keyword (CSS Text Decoration 3 section 3): a layer carrying `inset` is invalid, and a
// list with any invalid entry is invalid as a whole, like an invalid CSS declaration. `none` and a
// missing property both yield no shadows.
TextShadowList parse_text_shadows(const style::ComputedStyle& style,
                                  const rhi::Color& current_color);

// How far the painted shadows reach beyond the text box on each side (px, local box space),
// accounting for offsets and blur. Zero when there is no text shadow. The reach is a property of
// the style alone - it does not depend on the glyphs - so the scene sync can fold it into the
// node's paint outsets and culling/damage stay correct without touching the ink box.
struct TextShadowOutsets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
    bool blurred = false; // some shadow has a blur radius
};
TextShadowOutsets text_shadow_outsets(const style::ComputedStyle& style);

// Forwards every command to `target` and, for the text runs it forwards, emits their shadows first
// and then the runs themselves. Wrapping the sink is what makes the shadow cover *every* run a text
// node emits - the single-line fast path, wrapped lines, inline spans, the ellipsized line, the IME
// preedit and the decoration path - without threading a shadow list through each of those call
// sites (the same seam `LcdTextSink` uses for LCD eligibility).
//
// Runs are buffered until `flush()` and emitted as one block: the lines of a wrapped paragraph and
// the slices of a span-attributed line are one visual text block, so every shadow has to sit behind
// every glyph of that block. Emitting each run's shadow immediately would let the next run's shadow
// darken the previous run's glyphs. The caller flushes once, after the node's text is complete.
//
// `opacity` is the node's effective opacity, already folded into `color` by the caller; the shadow
// colour is multiplied by it here so a faded group fades its shadow too. `reduce_effects` caps a
// blurred shadow at `kReducedTextShadowRings` rings.
class TextShadowSink final : public PaintSink {
public:
    TextShadowSink(PaintSink& target, TextShadowList shadows, float opacity, bool reduce_effects,
                   PaintStats& stats)
        : target_(&target),
          shadows_(shadows),
          opacity_(opacity),
          reduce_effects_(reduce_effects),
          stats_(&stats) {}

    // Emits the buffered block: every shadow of every buffered run, back to front, then the runs.
    // Idempotent, so a caller that flushes twice cannot double-paint.
    void flush();

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
    void draw_text(const TextDraw& run) override;
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

private:
    PaintSink* target_;
    TextShadowList shadows_;
    float opacity_ = 1.0f;
    bool reduce_effects_ = false;
    PaintStats* stats_ = nullptr;
    std::vector<TextDraw> pending_;
};

} // namespace tessera::ui::paint::detail
