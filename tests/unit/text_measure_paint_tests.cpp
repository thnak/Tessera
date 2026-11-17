// SPDX-License-Identifier: MIT
//
// Issue #314: a content-sized text box must agree with the glyph ink it contains.
//
// The defect was that layout and paint measured with two different sources. Layout used
// `StubTextMeasurer`, whose advance is a fixed `0.5 * font_size` per code point and knows nothing
// about the loaded face, while paint emitted real HarfBuzz glyphs. The two disagreed per string, so
// the auto-width box was ~1px narrower than its ink for one label and ~2px wider for another - the
// sign flipped with the string, which is what ruled out a constant inset.
//
// The contract this test pins, for a text leaf with a border and horizontal padding inside an
// auto-sized row:
//
//   1. the measurer's advance equals the advance paint shapes with   (one source, not two)
//   2. box width == advance + 2 * border + 2 * padding, to within the pixel snap (the box follows
//   the text)
//   3. the ink fits inside the padding box                            (it never reaches the border)
//
// Assertion 1 is the one that fails on the old code, and it is asserted against the *shaper* - the
// exact seam the glyph renderer draws from - not against the measurer's own output, so a measurer
// cannot satisfy it by being self-consistent.
//
// Deterministic: the face is the committed OFL fixture, never a system font, and no wall clock,
// locale, network or GPU is involved.

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/layout/pixel_snap.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_text_measurer.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::style::ComputedStyle;
using tessera::ui::text::FontFace;
using tessera::ui::text::FontTextMeasurer;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "text_measure_paint: %s\n", message);
    }
    return condition;
}

ComputedStyle style(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle result;
    for (const auto& [property, value] : values) {
        result.set(property, value);
    }
    return result;
}

// The leftmost and rightmost painted pixel column of a shaped run, relative to the pen origin. This
// is the FreeType coverage box the glyph atlas uploads, so it is the ink the user sees.
struct InkExtent {
    float left = 0.0f;
    float right = 0.0f;
};

InkExtent ink_extent(std::string_view text, const TextStyle& text_style,
                     const HarfBuzzShaper& shaper, const GlyphRasterizer& rasterizer) {
    auto pixel_size = static_cast<std::uint16_t>(std::lround(text_style.font_size));
    if (pixel_size == 0) {
        pixel_size = 1;
    }
    float left = std::numeric_limits<float>::infinity();
    float right = -std::numeric_limits<float>::infinity();
    float pen = 0.0f;
    for (const auto& glyph : shaper.shape(text, text_style).glyphs) {
        const auto bitmap = rasterizer.rasterize_glyph(glyph.id, pixel_size);
        const float origin = pen + glyph.x_offset + static_cast<float>(bitmap.bearing_x);
        if (bitmap.width != 0) {
            left = std::min(left, origin);
            right = std::max(right, origin + static_cast<float>(bitmap.width));
        }
        pen += glyph.advance;
    }
    if (!std::isfinite(left) || !std::isfinite(right)) {
        return {};
    }
    return InkExtent{left, right};
}

// One content-sized text leaf in an auto-width row: exactly the Button shape of issue #314 (border,
// horizontal padding, no explicit width), so the leaf's box is whatever its measure callback asked
// for plus its own insets.
struct Case {
    const char* text;
    float font_size;
    float padding;
};

LayoutResult layout_label(const LayoutEngine& engine, const Case& test) {
    const std::string size = std::to_string(test.font_size);
    const std::string padding = std::to_string(static_cast<int>(test.padding));
    LayoutInput label;
    label.id = 2;
    label.text = test.text;
    // The longhands are what the Yoga adapter reads; the cascade expands `border-width` into them.
    label.style = style({{"border-left-width", "1px"},
                         {"border-right-width", "1px"},
                         {"border-left-style", "solid"},
                         {"border-right-style", "solid"},
                         {"padding-left", padding.c_str()},
                         {"padding-right", padding.c_str()},
                         {"font-size", size.c_str()}});
    LayoutInput row;
    row.id = 1;
    row.style = style({{"display", "flex"},
                       {"flex-direction", "row"},
                       {"align-items", "flex-start"},
                       {"width", "400px"}});
    row.children.push_back(std::move(label));
    return engine.layout(row, LayoutViewport{400.0f, 200.0f});
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "text_measure_paint: missing font path\n");
        return 2;
    }
    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::fprintf(stderr, "text_measure_paint: cannot load fixture: %s\n",
                     loaded.error().message.c_str());
        return 2;
    }
    const FontFace face = *loaded;
    const auto measurer = std::make_shared<const FontTextMeasurer>(face);
    const LayoutEngine engine(measurer, 64);
    const GlyphRasterizer rasterizer(face);
    const HarfBuzzShaper shaper(face);

    // The gallery's own labels across the font sizes its theme tokens produce: body 14 / label 11,
    // and the 125% / 150% text-scale tokens 17.5 / 16.5 / 21. "AV" and "Hello world" are the
    // string-shape cases (kerning pair, and a long lowercase run with a descender).
    const std::vector<Case> cases{
        {"Primary", 14.0f, 12.0f},     {"Secondary", 14.0f, 12.0f}, {"Ghost", 14.0f, 12.0f},
        {"Danger", 14.0f, 12.0f},      {"Small", 11.0f, 8.0f},      {"Disabled", 11.0f, 8.0f},
        {"Hello world", 14.0f, 12.0f}, {"AV", 14.0f, 12.0f},        {"Primary", 17.5f, 12.0f},
        {"Primary", 21.0f, 12.0f},     {"Small", 16.5f, 8.0f},
    };

    constexpr float kBorder = 1.0f;
    int checked = 0;
    for (const Case& test : cases) {
        const LayoutResult result = layout_label(engine, test);
        if (!expect(!result.boxes.empty(), "the row and its label are laid out")) {
            return 3;
        }
        const auto& bounds = result.boxes.back().bounds;
        const auto snapped = tessera::ui::layout::snap_rect(bounds, 1.0f);

        TextStyle text_style;
        text_style.font_size = test.font_size;
        TextConstraints no_wrap;
        no_wrap.wrap = false;
        const auto metrics = measurer->measure(test.text, text_style, no_wrap);
        const float shaped = shaper.shape(test.text, text_style).advance;
        const InkExtent ink = ink_extent(test.text, text_style, shaper, rasterizer);

        // (1) One source. The measurer layout sizes boxes from and the shaper paint draws with must
        // be the same number; this is the assertion the stub measurer fails.
        if (!expect(std::abs(metrics.width - shaped) < 0.01f,
                    "the layout measurer advances exactly what the paint shaper draws")) {
            std::fprintf(stderr, "  `%s` at %.1fpx: measured %.2f, shaped %.2f\n", test.text,
                         test.font_size, metrics.width, shaped);
            return 4;
        }
        // (2) The box follows that advance. Layout is unrounded, so the box is the fractional
        // advance plus the insets, and the paint-time edge snap (`pixel_snap.cpp`) rounds each
        // edge to the nearest pixel: the snapped width is within one pixel of the exact one.
        const float expected = metrics.width + 2.0f * kBorder + 2.0f * test.padding;
        const float box = snapped.width;
        if (!expect(std::abs(box - expected) < 1.0f,
                    "box width follows advance + padding + border to within the pixel snap")) {
            std::fprintf(stderr, "  `%s` at %.1fpx: box %.2f, expected %.2f (advance %.2f)\n",
                         test.text, test.font_size, box, expected, metrics.width);
            return 5;
        }
        // (3) The ink fits inside the padding box, so however the label is aligned it can never
        // reach the border. This is what protects the user-visible symptom: a glyph's right side
        // bearing can exceed its advance (`y` in the fixture face does), so sizing the box from the
        // advance alone is not enough - the padding has to absorb that overhang.
        const float ink_span = ink.right - std::min(ink.left, 0.0f);
        const float padding_box = box - 2.0f * kBorder;
        if (!expect(ink_span <= padding_box + 0.001f,
                    "glyph ink fits inside the padding box (never reaches the border)")) {
            std::fprintf(stderr, "  `%s` at %.1fpx: ink spans %.2f, padding box %.2f\n", test.text,
                         test.font_size, ink_span, padding_box);
            return 6;
        }
        ++checked;
    }

    if (!expect(checked == static_cast<int>(cases.size()), "every case was checked")) {
        return 7;
    }

    // The determinism guard: the same input twice produces the same boxes, so a regression cannot
    // be hidden behind a floating-point coincidence that only shows up in one run.
    for (const Case& test : cases) {
        const LayoutResult first = layout_label(engine, test);
        const LayoutResult second = layout_label(engine, test);
        if (!expect(first.boxes.size() == second.boxes.size(), "layout is repeatable") ||
            !expect(first.boxes.back().bounds.width == second.boxes.back().bounds.width,
                    "the label box width is repeatable")) {
            return 8;
        }
    }

    std::printf("text_measure_paint: ok (%d cases)\n", checked);
    return 0;
}
