// SPDX-License-Identifier: MIT
//
// OpenType feature passing (issue #966, docs/design/text.md section 3.1): the three CSS parsers,
// the computed-style mapping, the measure and layout cache keys, and the real shaper (NotoSans has
// `tnum`, `pnum` and `zero`). Deterministic: only the fixture font, no system fonts.

#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/paint/text_layout_cache.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_features.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <string>

namespace {

using namespace tessera::ui::text;
using tessera::ui::style::ComputedStyle;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "font_features: " << message << "\n";
        ++failures;
    }
}

std::uint32_t value_of(const FontFeatures& features, const char (&t)[5]) {
    const std::uint32_t tag = font_feature_tag(t[0], t[1], t[2], t[3]);
    for (std::size_t i = 0; i < features.count; ++i) {
        if (features.items[i].tag == tag) {
            return features.items[i].value;
        }
    }
    return 0xFFFFFFFFU;
}

FontFeaturesId with_feature(FontFeaturesId base, std::uint32_t tag, std::uint32_t value) {
    FontFeatures list = resolve_font_features(base);
    list.set(tag, value);
    return intern_font_features(list);
}

void test_interning() {
    FontFeatures a;
    a.set(font_feature_tag('t', 'n', 'u', 'm'), 1);
    FontFeatures b = a;
    const FontFeaturesId first = intern_font_features(a);
    check(first.value != 0, "a non-empty list gets a non-zero id");
    check(intern_font_features(b) == first, "equal lists share one id");
    b.set(font_feature_tag('z', 'e', 'r', 'o'), 1);
    check(intern_font_features(b) != first, "distinct lists get distinct ids");
    check(resolve_font_features(first) == a, "an id resolves to its list");
    check(intern_font_features(FontFeatures{}).value == 0, "the empty list is id 0");
    check(resolve_font_features(FontFeaturesId{}).empty(), "id 0 resolves to the empty list");
    check(resolve_font_features(FontFeaturesId{0xFFFFFFU}).empty(), "an unknown id is empty");
    // Fill the table: the overflow list drops to id 0 instead of growing without bound.
    FontFeaturesId last;
    for (std::uint32_t i = 0; i < kMaxInternedFontFeatures + 2; ++i) {
        FontFeatures unique;
        unique.set(font_feature_tag('s', 's', '0', '1'), 1000U + i);
        last = intern_font_features(unique);
    }
    check(last.value == 0, "the interning table is bounded");
}

void test_parsers() {
    FontFeatures f;
    check(parse_font_variant_numeric("tabular-nums slashed-zero", f), "numeric parses");
    check(f.count == 2 && value_of(f, "tnum") == 1 && value_of(f, "zero") == 1, "numeric tags");

    FontFeatures bad;
    check(!parse_font_variant_numeric("tabular-nums bogus", bad) && bad.empty(),
          "an invalid numeric value is ignored as a whole");
    check(parse_font_variant_numeric("normal", bad) && bad.empty(), "normal adds nothing");
    check(!parse_font_variant_numeric("", bad), "empty is invalid");

    FontFeatures lig;
    check(parse_font_variant_ligatures("none", lig) && value_of(lig, "liga") == 0 &&
              value_of(lig, "clig") == 0 && value_of(lig, "calt") == 0,
          "ligatures none turns the ligature features off");
    FontFeatures lig2;
    check(parse_font_variant_ligatures("no-common-ligatures discretionary-ligatures", lig2) &&
              value_of(lig2, "liga") == 0 && value_of(lig2, "dlig") == 1,
          "ligature keywords");

    FontFeatures s;
    check(parse_font_feature_settings("'tnum', \"liga\" off, 'ss01' 3, \"zero\" on", s),
          "settings parse");
    check(s.count == 4 && value_of(s, "tnum") == 1 && value_of(s, "liga") == 0 &&
              value_of(s, "ss01") == 3 && value_of(s, "zero") == 1,
          "settings values");
    FontFeatures none;
    check(!parse_font_feature_settings("tnum", none) && none.empty(), "unquoted tag is invalid");
    check(!parse_font_feature_settings("'tnu'", none), "three-byte tag is invalid");
    check(!parse_font_feature_settings("'tnum' 1, bogus", none) && none.empty(),
          "one bad entry rejects the whole declaration");
    check(!parse_font_feature_settings("'tnum' -1", none), "negative value is invalid");
    check(parse_font_feature_settings("normal", none) && none.empty(), "settings normal");

    const FontFeatures merged = parse_font_features(FontVariantValues{
        .ligatures = "none", .numeric = "tabular-nums", .settings = "'liga' 1, 'tnum' 0"});
    check(value_of(merged, "liga") == 1, "a raw setting overrides the ligature keyword");
    check(value_of(merged, "tnum") == 0, "a raw setting overrides the numeric keyword");
    check(merged.count == 6, "overrides replace in place, one entry per tag");
    check(parse_font_features(FontVariantValues{}) == FontFeatures{}, "no property, no features");
}

void test_computed_style() {
    ComputedStyle computed;
    check(text_style_from(computed).features.value == 0, "default style has no features");
    computed.set("font-variant-numeric", "tabular-nums");
    computed.set("font-feature-settings", "'ss02'");
    const TextStyle style = text_style_from(computed);
    const FontFeatures resolved = resolve_font_features(style.features);
    check(value_of(resolved, "tnum") == 1 && value_of(resolved, "ss02") == 1,
          "text_style_from carries numeric variants and feature settings");
}

std::string joined(const tessera::ui::style::PropertyExpansionResult& r) {
    std::string out;
    for (const auto& d : r.declarations) {
        out += d.property + "=" + d.value + ";";
    }
    return out;
}

void test_variant_longhands() {
    FontFeatures f;
    check(parse_font_variant_caps("small-caps", f) && value_of(f, "smcp") == 1, "small-caps");
    FontFeatures all;
    check(parse_font_variant_caps("ALL-small-caps", all) && value_of(all, "smcp") == 1 &&
              value_of(all, "c2sc") == 1,
          "all-small-caps selects smcp and c2sc");
    FontFeatures petite;
    check(parse_font_variant_caps("all-petite-caps", petite) && value_of(petite, "pcap") == 1 &&
              value_of(petite, "c2pc") == 1,
          "all-petite-caps");
    FontFeatures bad;
    check(!parse_font_variant_caps("small-caps petite-caps", bad) && bad.empty(),
          "two caps keywords conflict");
    check(!parse_font_variant_caps("bogus", bad), "unknown caps keyword");
    check(!parse_font_variant_caps("tabular-nums", bad), "a keyword of another group is invalid");

    FontFeatures ea;
    check(parse_font_variant_east_asian("jis90 full-width ruby", ea) && value_of(ea, "jp90") == 1 &&
              value_of(ea, "fwid") == 1 && value_of(ea, "ruby") == 1,
          "east-asian variant, width and ruby combine");
    check(!parse_font_variant_east_asian("jis78 jis83", bad), "two east-asian variants conflict");
    check(!parse_font_variant_east_asian("full-width proportional-width", bad),
          "two east-asian widths conflict");
    FontFeatures pos;
    check(parse_font_variant_position("super", pos) && value_of(pos, "sups") == 1, "super");
    check(parse_font_variant_position("sub", pos) && value_of(pos, "subs") == 1, "sub");
    check(!parse_font_variant_position("sub super", bad), "sub and super conflict");
    FontFeatures lig;
    check(!parse_font_variant_ligatures("none common-ligatures", lig), "none stands alone");
    check(!parse_font_variant_ligatures("common-ligatures no-common-ligatures", lig),
          "a repeated ligature class conflicts");
}

void test_variant_shorthand() {
    const tessera::ui::style::PropertyRegistry registry;
    const auto expand = [&](const char* value) {
        return registry.expand(tessera::ui::style::Declaration{"font-variant", value, false, 0});
    };
    const auto small = expand("small-caps tabular-nums");
    check(small.diagnostics.empty() && small.declarations.size() == 5,
          "shorthand yields five longhands");
    check(joined(small) == "font-variant-ligatures=normal;font-variant-numeric=tabular-nums;"
                           "font-variant-caps=small-caps;font-variant-east-asian=normal;"
                           "font-variant-position=normal;",
          "omitted groups reset to normal: " + joined(small));
    check(joined(expand("none")).find("font-variant-ligatures=none;") == 0,
          "none is ligatures none");
    check(joined(expand("normal")).find("none") == std::string::npos, "normal resets everything");
    check(expand("small-caps petite-caps").declarations.empty() &&
              !expand("small-caps petite-caps").diagnostics.empty(),
          "a conflict drops the whole shorthand with a diagnostic");
    check(expand("bogus").declarations.empty(), "an unknown component drops the shorthand");
    check(expand("normal small-caps").declarations.empty(), "normal cannot combine");

    const auto longhand = registry.expand(
        tessera::ui::style::Declaration{"font-variant-caps", "small-caps petite-caps", false, 0});
    check(longhand.declarations.empty() && !longhand.diagnostics.empty(),
          "an invalid longhand is reported and dropped");
    check(
        registry.expand(tessera::ui::style::Declaration{"font-variant-position", "super", false, 0})
                .declarations.size() == 1,
        "a valid longhand is kept");
}

void test_variants_computed() {
    ComputedStyle computed;
    computed.set("font-variant-caps", "all-small-caps");
    computed.set("font-variant-east-asian", "jis04");
    computed.set("font-variant-position", "sub");
    const FontFeatures f = resolve_font_features(text_style_from(computed).features);
    check(value_of(f, "smcp") == 1 && value_of(f, "c2sc") == 1 && value_of(f, "jp04") == 1 &&
              value_of(f, "subs") == 1,
          "text_style_from carries caps, east-asian and position");

    // Ligatures stay off while letter-spacing is non-zero, unless the author turns them on.
    ComputedStyle spaced;
    spaced.set("letter-spacing", "2px");
    FontFeatures s = resolve_font_features(text_style_from(spaced).features);
    check(value_of(s, "liga") == 0 && value_of(s, "clig") == 0,
          "letter-spacing disables liga/clig");
    check(value_of(s, "calt") == 0xFFFFFFFFU, "contextual alternates are left alone");
    spaced.set("font-feature-settings", "'liga' 1");
    s = resolve_font_features(text_style_from(spaced).features);
    check(value_of(s, "liga") == 1 && value_of(s, "clig") == 0, "an explicit setting wins");
    ComputedStyle normal_spacing;
    normal_spacing.set("letter-spacing", "normal");
    check(text_style_from(normal_spacing).features.value == 0, "zero spacing keeps defaults");
}

void test_intern_overflow_counter() {
    const std::uint64_t before = font_features_intern_dropped();
    FontFeatures unique;
    unique.set(font_feature_tag('s', 's', '1', '9'), 123456U);
    check(intern_font_features(unique).value == 0 && font_features_intern_dropped() == before + 1,
          "a dropped intern is counted");
}

class CountingMeasurer final : public TextMeasurer {
public:
    mutable std::size_t calls = 0;
    TextMetrics measure(std::string_view, const TextStyle&, const TextConstraints&) const override {
        ++calls;
        return {8.0f, 16.0f, 12.0f, 1};
    }
};

void test_cache_keys() {
    auto measurer = std::make_shared<CountingMeasurer>();
    TextMeasureCache cache(measurer, 8);
    TextStyle plain;
    TextStyle tabular;
    tabular.features = with_feature(tabular.features, font_feature_tag('t', 'n', 'u', 'm'), 1);
    const TextConstraints constraints;
    cache.measure("12", plain, constraints);
    cache.measure("12", plain, constraints);
    check(measurer->calls == 1, "same features hit the measure cache");
    cache.measure("12", tabular, constraints);
    check(measurer->calls == 2, "features are part of the measure cache key");
    cache.measure("12", tabular, constraints);
    check(measurer->calls == 2, "the featured entry hits");

    tessera::ui::paint::TextLayoutCache layout_cache;
    const tessera::ui::paint::StubPaintTextMetrics metrics;
    layout_cache.get("12", plain, constraints, metrics, 0);
    layout_cache.get("12", tabular, constraints, metrics, 0);
    layout_cache.get("12", tabular, constraints, metrics, 0);
    check(layout_cache.stats().misses == 2 && layout_cache.stats().hits == 1,
          "features are part of the layout cache key");
}

void test_shaper(const char* path) {
    const auto face = FontFace::load_from_file(path);
    if (!face) {
        check(false, "fixture font loads");
        return;
    }
    const HarfBuzzShaper shaper(*face);
    TextStyle plain;
    plain.font_size = 100.0f;
    TextStyle tabular = plain;
    tabular.features = with_feature(tabular.features, font_feature_tag('t', 'n', 'u', 'm'), 1);
    TextStyle proportional = plain;
    proportional.features =
        with_feature(proportional.features, font_feature_tag('p', 'n', 'u', 'm'), 1);

    const float one_default = shaper.shape("1", plain).advance;
    const float zero_default = shaper.shape("0", plain).advance;
    const float one_tabular = shaper.shape("1", tabular).advance;
    const float zero_tabular = shaper.shape("0", tabular).advance;
    const float one_proportional = shaper.shape("1", proportional).advance;
    check(one_default > 0.0f && zero_default > 0.0f, "digits shape");
    check(one_tabular == zero_tabular, "tnum makes every digit advance alike");
    check(one_proportional < zero_tabular, "pnum selects the narrower proportional one");

    ComputedStyle computed;
    computed.set("font-size", "100px");
    computed.set("font-variant-numeric", "tabular-nums");
    check(shaper.shape("1", text_style_from(computed)).advance == one_tabular,
          "font-variant-numeric reaches the shaper through text_style_from");

    TextStyle zero = plain;
    zero.features = with_feature(zero.features, font_feature_tag('z', 'e', 'r', 'o'), 1);
    check(shaper.shape("0", zero).glyphs[0].id != shaper.shape("0", plain).glyphs[0].id,
          "font-feature-settings 'zero' selects the slashed glyph");

    TextStyle off = tabular;
    off.features = with_feature(off.features, font_feature_tag('t', 'n', 'u', 'm'), 0);
    check(shaper.shape("1", off).advance == one_default, "tnum 0 turns the feature off again");
}

} // namespace

int main(int argc, char** argv) {
    test_parsers();
    test_computed_style();
    test_variant_longhands();
    test_variant_shorthand();
    test_variants_computed();
    test_cache_keys();
    if (argc > 1) {
        test_shaper(argv[1]);
    } else {
        check(false, "missing font path");
    }
    // Last: it fills the interning table on purpose.
    test_interning();
    test_intern_overflow_counter();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "font_features: ok\n";
    return 0;
}
