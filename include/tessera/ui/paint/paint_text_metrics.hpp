// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstdint>
#include <memory>
#include <string_view>

namespace tessera::ui::paint {

// Metrics-source id used by the deterministic stub source (the default). It is part of the paint
// text-layout cache key, so stub and real-font line boxes can never mix.
inline constexpr std::uint32_t kStubTextMetricsId = 0;

// The seam between paint's wrapping and the text stack. `layout` returns the line boxes (byte
// ranges, widths, heights, baselines) for a wrapped run; the paint pass emits those boxes verbatim.
//
// The default is `StubPaintTextMetrics`: deterministic stub metrics and the fixed-advance
// `text::layout_text` path, which keeps existing CPU goldens and tests byte-identical. A real-face
// source (`FontPaintTextMetrics`) uses FreeType metrics and HarfBuzz shaping advances, so wrapped
// lines match what the glyph renderer draws.
class PaintTextMetricsSource {
public:
    virtual ~PaintTextMetricsSource() = default;

    [[nodiscard]] virtual std::uint32_t id() const = 0;
    [[nodiscard]] virtual text::FontMetrics metrics(const text::TextStyle& style) const = 0;
    // Advance of one painted run in the same units/rounding the line layout uses, including the
    // `word-spacing` the wrap added for every separator in the run. Used to place caret and
    // selection boundaries inside a line box and to emit the painted segment sequence (paint never
    // shapes the full line again), so summing it over a line's segments equals the line width.
    [[nodiscard]] virtual float advance(std::string_view text,
                                        const text::TextStyle& style) const = 0;
    [[nodiscard]] virtual text::TextLayout
    layout(std::string_view text, const text::TextStyle& style,
           const text::TextConstraints& constraints) const = 0;
};

class StubPaintTextMetrics final : public PaintTextMetricsSource {
public:
    [[nodiscard]] std::uint32_t id() const override {
        return kStubTextMetricsId;
    }
    [[nodiscard]] text::FontMetrics metrics(const text::TextStyle& style) const override {
        return text::StubFontMetricsProvider{}.metrics(style);
    }
    [[nodiscard]] float advance(std::string_view text,
                                const text::TextStyle& style) const override {
        // The raw advance seam is literal: it must never trim or collapse, because the wrap adds
        // `advance(" ")` for a collapsed run and paint measures that same one-space segment with
        // this call. `word-spacing` is the one addition the wrap and the painted cursor share, so a
        // painted segment sequence sums to the line width; zero it for the literal measure.
        text::TextStyle literal = style;
        literal.white_space = text::WhiteSpace::Pre;
        literal.word_spacing = 0.0f;
        text::TextConstraints no_wrap;
        no_wrap.wrap = false;
        const float raw = layout(text, literal, no_wrap).width;
        const float spacing = style.word_spacing > 0.0f ? style.word_spacing : 0.0f;
        return raw + spacing * static_cast<float>(text::word_separator_count(text));
    }
    [[nodiscard]] text::TextLayout layout(std::string_view text, const text::TextStyle& style,
                                          const text::TextConstraints& constraints) const override {
        return text::layout_text(text, style, metrics(style), constraints);
    }
};

// Real-font source: FreeType line metrics from the loaded face plus HarfBuzz shaping advances for
// the line boxes. `id` must be stable per face/generation and distinct from other sources; the
// caller owns the source and invalidates the cache when the face changes.
class FontPaintTextMetrics final : public PaintTextMetricsSource {
public:
    FontPaintTextMetrics(text::FontFace face, std::uint32_t id);
    ~FontPaintTextMetrics() override;

    FontPaintTextMetrics(const FontPaintTextMetrics&) = delete;
    FontPaintTextMetrics& operator=(const FontPaintTextMetrics&) = delete;
    FontPaintTextMetrics(FontPaintTextMetrics&&) noexcept;
    FontPaintTextMetrics& operator=(FontPaintTextMetrics&&) noexcept;

    [[nodiscard]] std::uint32_t id() const override;
    [[nodiscard]] text::FontMetrics metrics(const text::TextStyle& style) const override;
    [[nodiscard]] float advance(std::string_view text, const text::TextStyle& style) const override;
    [[nodiscard]] text::TextLayout layout(std::string_view text, const text::TextStyle& style,
                                          const text::TextConstraints& constraints) const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::ui::paint
