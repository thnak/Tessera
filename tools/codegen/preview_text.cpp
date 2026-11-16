// SPDX-License-Identifier: MIT

#include "preview_text.hpp"
#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::cli {
namespace {

// Paint's text-layout cache keys its entries by this id. `kStubTextMetricsId` is 0 and belongs to
// the font-free path, so the real face takes the next value - the same choice the app window's
// `TextStack` makes (`app::detail::TextStack::kPaintMetricsId`).
constexpr std::uint32_t kPaintMetricsId = 1;

PreviewFontError font_error(text::FontErrorCode code, std::string message) {
    const PreviewFontErrorCode mapped = code == text::FontErrorCode::not_scalable
                                            ? PreviewFontErrorCode::not_scalable
                                            : PreviewFontErrorCode::invalid_font;
    return PreviewFontError{mapped, std::move(message)};
}

// The last acceptance check before the stack is handed out: shaping the one Latin letter every
// preview sample contains must produce a glyph with a positive advance.
//
// This is not belt-and-braces. A face whose `cmap` loads but whose outlines are unusable would
// leave every run empty, and an empty run paints nothing while `textSkipped` stays 0 - which reads
// on the page as "this source has no text" instead of "this font is broken". One shaped glyph at
// load time turns that into a named `TESSERA_PREVIEW_FONT` failure instead.
[[nodiscard]] bool shapes_ink(const text::TextShaper& shaper) {
    text::TextStyle style;
    style.font_size = 16.0f;
    const text::ShapedRun probe = shaper.shape("A", style);
    return !probe.glyphs.empty() && probe.glyphs.front().id != 0 && probe.advance > 0.0f;
}

} // namespace

std::expected<std::unique_ptr<PreviewTextStack>, PreviewFontError>
PreviewTextStack::create(std::span<const std::uint8_t> font_data, rendering::rhi::Device& device,
                         text::GlyphAtlasConfig atlas) {
    if (font_data.empty()) {
        return std::unexpected(
            PreviewFontError{PreviewFontErrorCode::empty_data, "no font bytes were supplied"});
    }
    // The production loader: `load_from_memory` copies the bytes into `FontFaceState`, so the
    // caller's buffer may be released as soon as this returns.
    std::expected<text::FontFace, text::FontError> loaded =
        text::FontFace::load_from_memory(font_data);
    if (!loaded) {
        return std::unexpected(font_error(loaded.error().code, loaded.error().message));
    }

    std::unique_ptr<PreviewTextStack> stack(new PreviewTextStack(std::move(*loaded)));
    stack->rasterizer_ = std::make_unique<text::GlyphRasterizer>(stack->face_);
    stack->shaper_ = std::make_unique<text::HarfBuzzShaper>(stack->face_);
    if (!shapes_ink(*stack->shaper_)) {
        return std::unexpected(PreviewFontError{
            PreviewFontErrorCode::unusable_face,
            "the font loaded but shapes no visible glyph for 'A', so the preview would paint no "
            "text at all"});
    }
    stack->renderer_ = std::make_unique<text::TextRenderer>(device, *stack->rasterizer_, atlas);
    // Layout and paint measure with the face that is drawn. The stub advance is a fixed 0.5 em per
    // code point, so a stub-sized box would disagree with its painted ink by a string-dependent
    // amount - the exact mismatch the preview exists to show.
    stack->measurer_ = std::make_shared<text::FontTextMeasurer>(stack->face_);
    stack->paint_metrics_ =
        std::make_unique<paint::FontPaintTextMetrics>(stack->face_, kPaintMetricsId);
    return stack;
}

PreviewTextStack::PreviewTextStack(text::FontFace face)
    : face_(std::move(face)) {}

PreviewTextStack::~PreviewTextStack() = default;

void PreviewTextStack::begin_frame() {
    renderer_->begin_frame();
}

PreviewTextCounters PreviewTextStack::counters() const {
    const text::GlyphAtlasStats& stats = renderer_->atlas().stats();
    PreviewTextCounters counters;
    counters.glyphs_dropped = stats.dropped;
    counters.atlas_pages = renderer_->atlas().page_count();
    counters.atlas_evictions = stats.evictions;
    return counters;
}

std::string PreviewTextStack::face_name() const {
    return face_.family_name();
}

} // namespace tessera::compiler::codegen::cli
