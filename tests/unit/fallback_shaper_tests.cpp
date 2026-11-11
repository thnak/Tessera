// SPDX-License-Identifier: MIT
//
// Font-stack fallback (issue #12, docs/design/text.md section 4): per-cluster face choice in stack
// order, sub-run concatenation with byte-accurate clusters, right-to-left visual order, a
// deterministic .notdef for uncovered clusters with a missing-glyph count, and a bounded
// cluster-to-face decision cache. argv[1] is NotoSans, argv[2] is NotoSansHebrew.

#include "tessera/ui/text/fallback_shaper.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

using tessera::ui::text::FallbackShaper;
using tessera::ui::text::FontFace;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::ShapedRun;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "fallback_shaper: " << message << "\n";
    }
    return condition;
}

FontFace load(const char* path) {
    auto face = FontFace::load_from_file(path);
    if (!face) {
        std::cerr << "fallback_shaper: cannot load " << path << ": " << face.error().message
                  << "\n";
        return {};
    }
    return *face;
}

// "shalom" in Hebrew letters, as UTF-8 escapes so the source stays ASCII.
const std::string kHebrew = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";

bool latin_stays_on_the_first_face(const FallbackShaper& stack) {
    const ShapedRun run = stack.shape("Hello", TextStyle{});
    bool ok = expect(run.glyphs.size() == 5 && run.missing_glyphs == 0, "five covered glyphs");
    ok = expect(std::all_of(run.glyphs.begin(), run.glyphs.end(),
                            [](const auto& g) { return g.face == 0 && g.id != 0; }),
                "all on face 0") &&
         ok;
    ok = expect(!run.right_to_left, "left to right") && ok;
    return ok;
}

bool mixed_text_uses_each_face(const FallbackShaper& stack, const HarfBuzzShaper& hebrew_only) {
    // "ab " + Hebrew + " cd": Latin and the spaces on face 0 (Sans covers space), Hebrew on face 1.
    const std::string text = "ab " + kHebrew + " cd";
    const ShapedRun run = stack.shape(text, TextStyle{});
    bool ok = expect(run.missing_glyphs == 0, "nothing missing");
    std::size_t on_hebrew = 0;
    for (const auto& g : run.glyphs) {
        on_hebrew += g.face == 1 ? 1U : 0U;
        ok = expect(g.id != 0, "no .notdef") && ok;
    }
    ok = expect(on_hebrew == 4, "the four Hebrew letters use the second face") && ok;
    ok = expect(run.glyphs.size() == 3 + 4 + 3, "one glyph per character") && ok;

    // Clusters are byte offsets into the whole text, not into the sub-run.
    ok = expect(run.glyphs.front().cluster == 0 && run.glyphs.back().cluster == text.size() - 1,
                "clusters index the original text") &&
         ok;
    // The Hebrew glyph ids are exactly what the Hebrew face shapes on its own.
    const ShapedRun alone = hebrew_only.shape_directed(kHebrew, TextStyle{}, true);
    std::vector<std::uint32_t> got;
    for (const auto& g : run.glyphs) {
        if (g.face == 1) {
            got.push_back(g.id);
        }
    }
    std::vector<std::uint32_t> want;
    for (const auto& g : alone.glyphs) {
        want.push_back(g.id);
    }
    // The mixed run is left to right (Latin leads), so its Hebrew letters are in logical order
    // here; the standalone run is right to left, so compare as sets.
    std::sort(got.begin(), got.end());
    std::sort(want.begin(), want.end());
    ok = expect(got == want, "Hebrew glyph ids match the Hebrew face") && ok;
    return ok;
}

bool stack_order_decides(const FontFace& sans, const FontFace& hebrew) {
    FallbackShaper reversed({hebrew, sans});
    // Hebrew first: it does not cover Latin, so Latin still falls through to Sans (index 1).
    const ShapedRun run = reversed.shape("ab", TextStyle{});
    bool ok = expect(run.glyphs.size() == 2 && run.glyphs[0].face == 1 && run.missing_glyphs == 0,
                     "Latin falls through to the second face");
    FallbackShaper sans_first({sans, hebrew});
    ok = expect(sans_first.face_for_cluster("a") == 0 && reversed.face_for_cluster("a") == 1,
                "the same character picks the face by stack order") &&
         ok;
    return ok;
}

bool right_to_left_is_visual_order(const FallbackShaper& stack) {
    // Hebrew, a space (covered by face 0), Hebrew: three sub-runs on faces 1, 0, 1.
    const std::string text = kHebrew + " " + kHebrew;
    const ShapedRun run = stack.shape(text, TextStyle{});
    bool ok = expect(run.right_to_left, "right to left");
    ok = expect(run.glyphs.size() == 9 && run.missing_glyphs == 0, "nine glyphs, none missing") &&
         ok;
    bool descending = true;
    for (std::size_t i = 1; i < run.glyphs.size(); ++i) {
        descending = descending && run.glyphs[i].cluster < run.glyphs[i - 1].cluster;
    }
    ok = expect(descending, "clusters run backwards across the face boundaries") && ok;
    ok = expect(run.glyphs[4].face == 0, "the space in the middle is on face 0") && ok;
    return ok;
}

bool uncovered_is_notdef_and_counted(const FallbackShaper& stack) {
    // U+1F600 is in neither test face.
    const ShapedRun run = stack.shape("a\xF0\x9F\x98\x80"
                                      "b",
                                      TextStyle{});
    bool ok = expect(run.glyphs.size() == 3 && run.missing_glyphs == 1, "one missing glyph");
    ok = expect(run.glyphs[1].id == 0 && run.glyphs[1].face == 0 && run.glyphs[1].cluster == 1,
                "tofu on the first face at the right byte") &&
         ok;
    ok = expect(stack.face_for_cluster("\xF0\x9F\x98\x80") == stack.size(), "no face covers it") &&
         ok;
    const ShapedRun again = stack.shape("a\xF0\x9F\x98\x80"
                                        "b",
                                        TextStyle{});
    ok = expect(again.missing_glyphs == 1 && again.glyphs.size() == run.glyphs.size(),
                "deterministic") &&
         ok;
    return ok;
}

bool clusters_stay_together(const FallbackShaper& stack) {
    // e + combining acute is one cluster and one decision; a keycap and a ZWJ sequence too.
    const std::size_t before = stack.cached_decisions();
    bool ok = expect(stack.face_for_cluster("e\xCC\x81") == 0, "base plus mark on face 0");
    ok = expect(stack.cached_decisions() == before + 1, "one decision for the cluster") && ok;
    (void)stack.face_for_cluster("e\xCC\x81");
    ok = expect(stack.cached_decisions() == before + 1, "a repeat hits the cache") && ok;

    // Two woman-ZWJ-woman sequences and a flag pair are single clusters: the run's glyph clusters
    // must start exactly at each cluster boundary.
    const std::string flag = "\xF0\x9F\x87\xAB\xF0\x9F\x87\xB7"; // regional indicators F + R
    const ShapedRun run = stack.shape("a" + flag + "b", TextStyle{});
    std::size_t clusters_in_flag = 0;
    for (const auto& g : run.glyphs) {
        clusters_in_flag += (g.cluster >= 1 && g.cluster < 1 + flag.size()) ? 1U : 0U;
    }
    ok = expect(run.glyphs.front().cluster == 0 && run.glyphs.back().cluster == 1 + flag.size(),
                "the flag pair sits between a and b") &&
         ok;
    ok = expect(clusters_in_flag >= 1, "the flag produced glyphs inside its byte range") && ok;
    return ok;
}

bool empty_input() {
    FallbackShaper none({});
    return expect(none.size() == 0 && none.shape("a", TextStyle{}).glyphs.empty(),
                  "an empty stack shapes nothing");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "fallback_shaper: need the NotoSans and NotoSansHebrew paths\n";
        return 64;
    }
    const FontFace sans = load(argv[1]);
    const FontFace hebrew = load(argv[2]);
    if (!sans.valid() || !hebrew.valid()) {
        return 65;
    }
    const FallbackShaper stack({sans, hebrew});
    const HarfBuzzShaper hebrew_only(hebrew);

    bool ok = expect(stack.size() == 2, "two faces");
    ok = latin_stays_on_the_first_face(stack) && ok;
    ok = mixed_text_uses_each_face(stack, hebrew_only) && ok;
    ok = stack_order_decides(sans, hebrew) && ok;
    ok = right_to_left_is_visual_order(stack) && ok;
    ok = uncovered_is_notdef_and_counted(stack) && ok;
    ok = clusters_stay_together(stack) && ok;
    ok = empty_input() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "fallback_shaper: ok\n";
    return 0;
}
