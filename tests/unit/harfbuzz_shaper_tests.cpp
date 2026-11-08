// SPDX-License-Identifier: MIT

#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/freetype_metrics.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {

using tessera::ui::text::FontFace;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::ShapedRun;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "harfbuzz_shaper: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs, float tolerance = 0.001f) {
    return std::abs(lhs - rhs) <= tolerance;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "harfbuzz_shaper: missing font path\n";
        return 1;
    }

    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "harfbuzz_shaper: cannot load fixture: " << loaded.error().message << "\n";
        return 1;
    }

    const HarfBuzzShaper shaper(*loaded);
    TextStyle style;
    style.font_size = 16.0f;

    const ShapedRun a = shaper.shape("A", style);
    if (!expect(a.glyphs.size() == 1, "single glyph for A") ||
        !expect(a.glyphs[0].id != 0, "glyph id for A") ||
        !expect(a.glyphs[0].cluster == 0, "cluster for A") ||
        !expect(a.glyphs[0].advance > 0.0f, "advance for A") ||
        !expect(near(a.advance, a.glyphs[0].advance), "run advance") ||
        !expect(!a.right_to_left, "Latin run is left-to-right")) {
        return 2;
    }

    const tessera::ui::text::FontMetrics metrics =
        tessera::ui::text::FreeTypeFontMetricsProvider(*loaded).metrics(style);
    if (!expect(near(a.ascent, metrics.ascent), "shaped ascent") ||
        !expect(near(a.descent, metrics.descent), "shaped descent")) {
        return 3;
    }

    const ShapedRun av = shaper.shape("AV", style);
    const ShapedRun v = shaper.shape("V", style);
    if (!expect(av.glyphs.size() == 2, "two glyphs for AV") ||
        !expect(av.advance + 0.01f < a.advance + v.advance, "kerning tightens AV")) {
        return 4;
    }

    const ShapedRun accented = shaper.shape("a\u00E9", style);
    if (!expect(accented.glyphs.size() == 2, "two glyphs for a+accented e") ||
        !expect(accented.glyphs[1].cluster == 1, "cluster is a byte offset")) {
        return 5;
    }

    const ShapedRun ligature = shaper.shape("ffi", style);
    if (!expect(ligature.glyphs.size() <= 3, "ffi shapes") ||
        !expect(ligature.glyphs.size() < 3, "ffi ligates")) {
        return 6;
    }

    // U+10FFFF as UTF-8 bytes; MSVC warns on the universal-character-name in a narrow literal.
    const std::string_view missing_text = "\xF4\x8F\xBF\xBF";
    const ShapedRun missing = shaper.shape(missing_text, style);
    if (!expect(missing.glyphs.size() == 1, "missing glyph shapes to one glyph") ||
        !expect(missing.glyphs[0].id == 0, "missing glyph is .notdef")) {
        return 7;
    }

    TextStyle larger = style;
    larger.font_size = 32.0f;
    const ShapedRun scaled = shaper.shape("AV", larger);
    if (!expect(near(scaled.advance, av.advance * 2.0f), "advance scales with font size")) {
        return 8;
    }

    const ShapedRun empty = shaper.shape("", style);
    if (!expect(empty.glyphs.empty(), "empty text shapes to no glyphs") ||
        !expect(empty.advance == 0.0f, "empty text has no advance")) {
        return 9;
    }

    const HarfBuzzShaper invalid{FontFace{}};
    const ShapedRun none = invalid.shape("A", style);
    if (!expect(none.glyphs.empty(), "invalid face shapes to no glyphs")) {
        return 10;
    }

    std::cout << "harfbuzz_shaper: ok\n";
    return 0;
}
