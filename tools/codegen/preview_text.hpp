// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_text_measurer.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_renderer.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

// The preview's text stack (issue #387): the real `src/ui/text/` engine, loaded from **font bytes
// the caller supplies** rather than from a file or a system font.
//
// **Why bytes and not a path.** The preview runs both natively and in a browser, and the browser
// module has no filesystem (docs/design/compiler-wasm.md section 2.1 keeps `-sNODERAWFS` off on
// purpose). The page fetches one font file from its own origin and hands the module the bytes
// through `tessera_canvas_set_font`; the native CLI reads the same shipped file and hands this the
// same bytes. One font, one set of bytes, one stack, so the two lanes cannot measure or rasterize
// differently.
//
// **Why this is not a second text path.** It owns no measurement, shaper, atlas or renderer of its
// own: `FontFace` is the production loader, `FontTextMeasurer` and `FontPaintTextMetrics` are the
// production seams `src/app/window_internal.hpp`'s `TextStack` installs, `HarfBuzzShaper` is the
// production shaper and `TextRenderer` is the production atlas-quad batcher. This class is only the
// *lifetime* that ties them to one set of bytes, exactly as `TextStack` does for the app window.
//
// **Determinism.** Nothing here consults a system font, a fallback chain, a clock or the locale:
// the face is the bytes that were handed in, and a face that fails to load leaves the preview in
// its font-free state (boxes painted, runs counted in `text_skipped`). The same bytes therefore
// produce the same pixels on every machine, which is what lets a golden exist at all.
namespace tessera::compiler::codegen::cli {

// The text stack and the RHI live one namespace up, and this file names them constantly; the
// aliases keep the declarations below readable without a `using namespace` (which no header here
// uses).
namespace rhi = tessera::rendering::rhi;
namespace paint = tessera::ui::paint;
namespace text = tessera::ui::text;

// Why a font could not be used. `invalid_font`/`not_scalable` are `FontFace`'s own codes; the other
// two are decided here.
enum class PreviewFontErrorCode : std::uint8_t {
    empty_data,
    invalid_font,
    not_scalable,
    unusable_face,
};

struct PreviewFontError {
    PreviewFontErrorCode code = PreviewFontErrorCode::invalid_font;
    std::string message;
};

// What the bounded glyph atlas could not hold, sampled after a frame.
//
// The atlas is a fixed pool of `max_pages` pages of `page_width` x `page_height` pixels. When every
// page is full and none may be evicted - a page touched in the current frame is pinned, so a single
// frame can never evict the page it is still drawing from - `GlyphAtlas::get_glyph` returns an
// entry with `has_bitmap == false` that still carries the advance, and `TextRenderer` draws no quad
// for it. The consequence is precise and worth stating: the run is **laid out and advanced
// correctly**, so every following glyph, the line box and the text's measured width are unchanged,
// but that one glyph's ink is missing from the frame. `glyphs_dropped` counts it, so "a word is
// missing a letter" is an attributable fact rather than a stylesheet bug.
//
// Growth is deliberately not the answer: the atlas is bounded to keep the preview's memory a
// function of the viewport rather than of the source text (`docs/design/reference-equantic-ui.md`
// section 7.4, and the P6 budget it feeds).
struct PreviewTextCounters {
    // Glyph bitmaps the atlas could not place this frame (`GlyphAtlasStats::dropped`).
    std::uint64_t glyphs_dropped = 0;
    // Pages the atlas ever allocated (bounded by the config's `max_pages`).
    std::uint64_t atlas_pages = 0;
    // Whole-page evictions, which happen between frames only.
    std::uint64_t atlas_evictions = 0;
};

// One loaded face plus the rasterizer/shaper/renderer trio.
//
// Member order is load-bearing and matches `app::detail::TextStack`: `face` is constructed first,
// `rasterizer` borrows it, `renderer` borrows the rasterizer, and the measurers hold their own
// `FontFace` copy (which shares the FreeType face through `FontFaceState`'s `shared_ptr`), so a
// layout engine's `shared_ptr` to the measurer can outlive this object.
//
// Not copyable and not movable: `TextRenderer` keeps a raw pointer to the rasterizer, so a move
// would leave it pointing at the moved-from object. The owner holds it behind a `unique_ptr`.
class PreviewTextStack final {
public:
    // Loads `font_data` (TTF/OTF bytes) and builds the stack. `device` is borrowed and must outlive
    // the stack. The atlas defaults are the app window's (`1024x1024`, two pages), so the preview
    // and the native window have one bounded budget rather than two.
    [[nodiscard]] static std::expected<std::unique_ptr<PreviewTextStack>, PreviewFontError>
    create(std::span<const std::uint8_t> font_data, rhi::Device& device,
           text::GlyphAtlasConfig atlas = {1024, 1024, 2, 1});

    ~PreviewTextStack();

    PreviewTextStack(const PreviewTextStack&) = delete;
    PreviewTextStack& operator=(const PreviewTextStack&) = delete;
    PreviewTextStack(PreviewTextStack&&) = delete;
    PreviewTextStack& operator=(PreviewTextStack&&) = delete;

    // The shaper the paint sink draws with, and the atlas-quad renderer behind it.
    [[nodiscard]] text::TextShaper& shaper() noexcept {
        return *shaper_;
    }
    [[nodiscard]] text::TextRenderer& renderer() noexcept {
        return *renderer_;
    }
    // The layout measurer: the face itself, so a content-sized box agrees with its painted ink
    // (issue #314, and the reason the font-free stub is not acceptable for a preview).
    [[nodiscard]] const std::shared_ptr<const text::TextMeasurer>& measurer() const noexcept {
        return measurer_;
    }
    // Paint's wrapping source, with the same face and a non-stub id so the text-layout cache can
    // never mix stub and real boxes.
    [[nodiscard]] const paint::PaintTextMetricsSource& paint_metrics() const noexcept {
        return *paint_metrics_;
    }

    // Called once per painted frame, before any draw, so the atlas pins the pages this frame uses.
    void begin_frame();
    // The counters above, sampled after the frame's draws.
    [[nodiscard]] PreviewTextCounters counters() const;
    // The face's own family name, for the page's font status. Empty on an unusable face.
    [[nodiscard]] std::string face_name() const;

private:
    explicit PreviewTextStack(text::FontFace face);

    text::FontFace face_;
    std::unique_ptr<text::GlyphRasterizer> rasterizer_;
    std::unique_ptr<text::HarfBuzzShaper> shaper_;
    std::unique_ptr<text::TextRenderer> renderer_;
    std::shared_ptr<const text::TextMeasurer> measurer_;
    std::unique_ptr<paint::FontPaintTextMetrics> paint_metrics_;
};

} // namespace tessera::compiler::codegen::cli
