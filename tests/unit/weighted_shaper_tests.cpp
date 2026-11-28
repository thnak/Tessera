// SPDX-License-Identifier: MIT
//
// Weight-aware face selection (issue #973): the weight a font file declares, CSS weight matching
// over the loaded faces, the shaper picking a face per run (`Glyph::face` is the index the
// renderer rasterizes with), and the layout/paint measurers following the same choice so a bold
// label is as wide as the bold glyphs drawn. argv[1] is Plex Sans Regular, argv[2] Bold, argv[3]
// SemiBold.

#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_text_measurer.hpp"
#include "tessera/ui/text/weighted_shaper.hpp"

#include <array>
#include <iostream>
#include <vector>

namespace {

using tessera::ui::text::FontFace;
using tessera::ui::text::FontTextMeasurer;
using tessera::ui::text::match_font_weight;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextStyle;
using tessera::ui::text::WeightedShaper;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "weighted_shaper: " << message << "\n";
    }
    return condition;
}

FontFace load(const char* path) {
    auto face = FontFace::load_from_file(path);
    if (!face) {
        std::cerr << "weighted_shaper: cannot load " << path << ": " << face.error().message
                  << "\n";
        return {};
    }
    return *face;
}

TextStyle styled(int weight) {
    TextStyle style;
    style.font_size = 16.0f;
    style.font_weight = weight;
    return style;
}

bool matching_follows_css() {
    const std::array<int, 2> regular_bold = {400, 700};
    const std::array<int, 3> with_semi = {400, 700, 600};
    const std::array<int, 1> only_regular = {400};
    const std::array<int, 2> light_bold = {300, 700};
    bool ok = expect(match_font_weight(regular_bold, 400) == 0, "400 is the regular face");
    ok = expect(match_font_weight(regular_bold, 700) == 1, "700 is the bold face") && ok;
    ok = expect(match_font_weight(regular_bold, 0) == 0, "an unset weight is 400") && ok;
    ok = expect(match_font_weight(regular_bold, 500) == 0, "500 prefers lighter over 700") && ok;
    ok = expect(match_font_weight(regular_bold, 600) == 1, "600 goes heavier first") && ok;
    ok = expect(match_font_weight(regular_bold, 100) == 0, "100 goes lighter then heavier") && ok;
    ok = expect(match_font_weight(with_semi, 600) == 2, "an exact 600 wins") && ok;
    ok = expect(match_font_weight(with_semi, 800) == 1, "800 falls back to the heaviest") && ok;
    ok = expect(match_font_weight(only_regular, 700) == 0, "a lone face serves every weight") && ok;
    ok = expect(match_font_weight(light_bold, 400) == 0, "400 with only 300 and 700 takes 300") &&
         ok;
    ok = expect(match_font_weight({}, 400) == 0, "an empty set is index 0") && ok;
    return ok;
}

bool declared_weights(const FontFace& regular, const FontFace& bold, const FontFace& semi) {
    bool ok = expect(regular.weight() == 400, "regular declares 400");
    ok = expect(bold.weight() == 700, "bold declares 700") && ok;
    ok = expect(semi.weight() == 600, "semibold declares 600") && ok;
    ok = expect(FontFace{}.weight() == 400, "an invalid face reports 400") && ok;
    return ok;
}

bool shaper_picks_the_face(const FontFace& regular, const FontFace& bold) {
    const WeightedShaper shaper({regular, bold});
    const auto normal = shaper.shape("Label", styled(400));
    const auto heavy = shaper.shape("Label", styled(700));
    bool ok = expect(shaper.size() == 2, "two faces");
    ok = expect(!normal.glyphs.empty() && normal.glyphs.size() == heavy.glyphs.size(),
                "same glyph count") &&
         ok;
    for (const auto& glyph : normal.glyphs) {
        ok = expect(glyph.face == 0, "weight 400 draws with face 0") && ok;
    }
    for (const auto& glyph : heavy.glyphs) {
        ok = expect(glyph.face == 1, "weight 700 draws with face 1") && ok;
    }
    ok = expect(heavy.advance > normal.advance, "bold text is wider than regular") && ok;

    const WeightedShaper single({regular});
    const auto one = single.shape("Label", styled(700));
    ok = expect(one.advance == normal.advance, "a single face ignores the weight") && ok;
    return ok;
}

bool measurers_follow_the_face(const FontFace& regular, const FontFace& bold) {
    const FontTextMeasurer family(std::vector<FontFace>{regular, bold});
    const FontTextMeasurer single(regular);
    TextConstraints constraints;
    constraints.wrap = false;
    const float normal = family.measure("Save changes", styled(400), constraints).width;
    const float heavy = family.measure("Save changes", styled(700), constraints).width;
    bool ok = expect(heavy > normal, "layout measures bold wider");
    ok = expect(single.measure("Save changes", styled(700), constraints).width == normal,
                "one face measures every weight alike") &&
         ok;

    const tessera::ui::paint::FontPaintTextMetrics paint(std::vector<FontFace>{regular, bold}, 1);
    ok = expect(paint.layout("Save changes", styled(700), constraints).width == heavy,
                "paint layout equals the measurer for bold") &&
         ok;
    ok = expect(paint.layout("Save changes", styled(400), constraints).width == normal,
                "paint layout equals the measurer for regular") &&
         ok;
    ok = expect(paint.advance("Save changes", styled(700)) >
                    paint.advance("Save changes", styled(400)),
                "paint advances bold wider") &&
         ok;
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "usage: weighted_shaper_tests <regular> <bold> <semibold>\n";
        return 2;
    }
    const FontFace regular = load(argv[1]);
    const FontFace bold = load(argv[2]);
    const FontFace semi = load(argv[3]);
    if (!regular || !bold || !semi) {
        return 2;
    }
    bool ok = matching_follows_css();
    ok = declared_weights(regular, bold, semi) && ok;
    ok = shaper_picks_the_face(regular, bold) && ok;
    ok = measurers_follow_the_face(regular, bold) && ok;
    if (ok) {
        std::cout << "weighted_shaper: ok\n";
    }
    return ok ? 0 : 1;
}
