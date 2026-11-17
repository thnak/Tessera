// SPDX-License-Identifier: MIT

// AaPolicy resolver (display-aware-rendering.md sections 2, 5, 7): hinting and subpixel positioning
// by render scale, the LCD gate, the power and AA-off presets, and golden mode ignoring every
// input.

#include "tessera/hal/window.hpp"
#include "tessera/ui/scale/aa_policy.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using tessera::hal::Orientation;
using tessera::hal::PixelGeometry;
using tessera::ui::scale::AaConfig;
using tessera::ui::scale::AaFacts;
using tessera::ui::scale::AaPolicy;
using tessera::ui::scale::EdgeMode;
using tessera::ui::scale::TextAaMode;
using tessera::ui::scale::TextAaSetting;
using tessera::ui::scale::TextHinting;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "aa_policy: " << message << "\n";
    }
    return condition;
}

AaFacts facts_at(float scale, PixelGeometry geometry = PixelGeometry::Unknown) {
    AaFacts facts;
    facts.render_scale = scale;
    facts.pixel_geometry = geometry;
    return facts;
}

bool hinting_and_positioning_follow_scale() {
    const AaConfig config;
    struct Row {
        float scale;
        TextHinting hinting;
        bool subpixel_position;
    };
    constexpr std::array<Row, 4> rows{{
        {1.0f, TextHinting::Light, false},
        {1.25f, TextHinting::Light, true},
        {1.5f, TextHinting::Light, true},
        {2.0f, TextHinting::None, true},
    }};
    bool ok = true;
    for (const Row& row : rows) {
        const AaPolicy policy = tessera::ui::scale::resolve_aa_policy(facts_at(row.scale), config);
        ok &= expect(policy.render_scale == row.scale, "ladder step is kept");
        ok &= expect(policy.hinting == row.hinting, "hinting by scale");
        ok &= expect(policy.subpixel_position == row.subpixel_position, "subpixel by scale");
        ok &= expect(policy.edge_mode == EdgeMode::ScreenSpace, "default edge is screen-space");
        ok &= expect(policy.clip_aa, "clip AA on by default");
    }
    return ok;
}

bool off_ladder_and_invalid_scales_resolve_safely() {
    const AaConfig config;
    bool ok = true;
    ok &= expect(tessera::ui::scale::resolve_aa_policy(facts_at(1.1667f), config).render_scale ==
                     1.25f,
                 "1.1667 quantizes to 1.25");
    ok &=
        expect(tessera::ui::scale::resolve_aa_policy(facts_at(-1.0f), config).render_scale == 1.0f,
               "negative scale falls back to 1.0");
    ok &= expect(tessera::ui::scale::resolve_aa_policy(
                     facts_at(std::numeric_limits<float>::quiet_NaN()), config)
                         .render_scale == 1.0f,
                 "NaN scale falls back to 1.0");
    return ok;
}

bool auto_text_stays_grayscale_until_lcd_is_decided() {
    AaConfig config;
    config.text_aa = TextAaSetting::Auto;
    const AaPolicy policy =
        tessera::ui::scale::resolve_aa_policy(facts_at(1.0f, PixelGeometry::RgbHorizontal), config);
    return expect(policy.text_mode == TextAaMode::Grayscale, "auto does not choose LCD yet");
}

bool explicit_subpixel_needs_every_lcd_fact() {
    AaConfig config;
    config.text_aa = TextAaSetting::Subpixel;
    bool ok = true;

    for (const PixelGeometry geometry :
         {PixelGeometry::RgbHorizontal, PixelGeometry::BgrHorizontal}) {
        ok &= expect(
            tessera::ui::scale::resolve_aa_policy(facts_at(1.0f, geometry), config).text_mode ==
                TextAaMode::LcdWhenEligible,
            "horizontal stripe permits LCD");
    }
    for (const PixelGeometry geometry : {PixelGeometry::Unknown, PixelGeometry::None,
                                         PixelGeometry::RgbVertical, PixelGeometry::BgrVertical}) {
        ok &= expect(
            tessera::ui::scale::resolve_aa_policy(facts_at(1.0f, geometry), config).text_mode ==
                TextAaMode::Grayscale,
            "unknown, none and vertical stripes stay grayscale");
    }

    AaFacts translucent = facts_at(1.0f, PixelGeometry::RgbHorizontal);
    translucent.transparent_window = true;
    ok &= expect(tessera::ui::scale::resolve_aa_policy(translucent, config).text_mode ==
                     TextAaMode::Grayscale,
                 "translucent window is grayscale");

    AaFacts portrait = facts_at(1.0f, PixelGeometry::RgbHorizontal);
    portrait.orientation = Orientation::Portrait;
    ok &= expect(tessera::ui::scale::resolve_aa_policy(portrait, config).text_mode ==
                     TextAaMode::Grayscale,
                 "rotated display is grayscale");

    config.text_aa = TextAaSetting::Grayscale;
    ok &= expect(
        tessera::ui::scale::resolve_aa_policy(facts_at(1.0f, PixelGeometry::RgbHorizontal), config)
                .text_mode == TextAaMode::Grayscale,
        "explicit grayscale wins");
    return ok;
}

bool reduced_quality_and_aa_off_presets() {
    AaConfig reduced;
    reduced.text_aa = TextAaSetting::Subpixel;
    reduced.msaa_samples = 4;
    reduced.reduced_quality = true;
    const AaPolicy cheap = tessera::ui::scale::resolve_aa_policy(
        facts_at(1.0f, PixelGeometry::RgbHorizontal), reduced);

    AaConfig off;
    off.aa_enabled = false;
    off.msaa_samples = 4;
    const AaPolicy hard = tessera::ui::scale::resolve_aa_policy(facts_at(1.0f), off);

    AaConfig msaa;
    msaa.msaa_samples = 4;
    AaConfig odd;
    odd.msaa_samples = 8;

    bool ok = true;
    ok &= expect(cheap.text_mode == TextAaMode::Grayscale && cheap.msaa_samples == 1,
                 "power preset forces grayscale and one sample");
    ok &= expect(hard.edge_mode == EdgeMode::Off && !hard.clip_aa && hard.msaa_samples == 1,
                 "AA off gives hard edges and one sample");
    ok &= expect(tessera::ui::scale::resolve_aa_policy(facts_at(1.0f), msaa).msaa_samples == 4,
                 "4x MSAA passes through");
    ok &= expect(tessera::ui::scale::resolve_aa_policy(facts_at(1.0f), odd).msaa_samples == 1,
                 "unsupported sample count resolves to 1");
    return ok;
}

bool golden_mode_ignores_every_input() {
    AaConfig config;
    config.golden_mode = true;
    config.text_aa = TextAaSetting::Subpixel;
    config.msaa_samples = 4;
    AaFacts facts = facts_at(2.0f, PixelGeometry::RgbHorizontal);

    const AaPolicy policy = tessera::ui::scale::resolve_aa_policy(facts, config);
    bool ok = expect(policy == tessera::ui::scale::golden_aa_policy(), "golden policy is fixed");
    ok &= expect(policy.render_scale == 1.0f && policy.hinting == TextHinting::Font &&
                     !policy.subpixel_position && policy.edge_mode == EdgeMode::Off &&
                     !policy.clip_aa && policy.msaa_samples == 1 &&
                     policy.text_mode == TextAaMode::Grayscale,
                 "golden policy values");
    return ok;
}

bool pixel_geometry_helpers() {
    bool ok = true;
    ok &=
        expect(tessera::hal::pixel_geometry_is_horizontal_stripe(PixelGeometry::RgbHorizontal) &&
                   tessera::hal::pixel_geometry_is_horizontal_stripe(PixelGeometry::BgrHorizontal),
               "horizontal stripes");
    ok &= expect(!tessera::hal::pixel_geometry_is_horizontal_stripe(PixelGeometry::RgbVertical) &&
                     !tessera::hal::pixel_geometry_is_horizontal_stripe(PixelGeometry::Unknown),
                 "other geometries are not usable stripes");
    ok &= expect(tessera::hal::pixel_geometry_is_known_value(PixelGeometry::BgrVertical),
                 "enumerator is a known value");
    ok &= expect(!tessera::hal::pixel_geometry_is_known_value(static_cast<PixelGeometry>(200)),
                 "wire value 200 is not an enumerator");
    ok &= expect(tessera::hal::WindowMetrics{}.pixel_geometry == PixelGeometry::Unknown,
                 "metrics default to Unknown");
    ok &= expect(tessera::hal::DisplayInfo{}.pixel_geometry == PixelGeometry::Unknown,
                 "display info defaults to Unknown");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= hinting_and_positioning_follow_scale();
    ok &= off_ladder_and_invalid_scales_resolve_safely();
    ok &= auto_text_stays_grayscale_until_lcd_is_decided();
    ok &= explicit_subpixel_needs_every_lcd_fact();
    ok &= reduced_quality_and_aa_off_presets();
    ok &= golden_mode_ignores_every_input();
    ok &= pixel_geometry_helpers();
    return ok ? 0 : 1;
}
