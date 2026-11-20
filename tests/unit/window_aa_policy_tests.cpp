// SPDX-License-Identifier: MIT
//
// The window's display-aware AA policy mapping (docs/design/display-aware-rendering.md sections 2,
// 5 and 6). It proves the resolver inputs are taken from the window metrics and the renderer
// config, that the explicit switch / an AA-off renderer / the renderer's golden flag all pin the
// golden policy regardless of the display, and that the power preset and MSAA rules reach the
// resolver. Pure CPU: no GPU, window or display.

#include "tessera/hal/display.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/scale/aa_policy.hpp"
#include "window_config.hpp"

#include <iostream>
#include <limits>

namespace {

using tessera::app::window_aa_facts;
using tessera::app::window_aa_policy;
using tessera::app::WindowAaOptions;
using tessera::hal::Orientation;
using tessera::hal::PixelGeometry;
using tessera::hal::WindowMetrics;
using tessera::ui::scale::AaPolicy;
using tessera::ui::scale::EdgeMode;
using tessera::ui::scale::golden_aa_policy;
using tessera::ui::scale::TextAaMode;
using tessera::ui::scale::TextHinting;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_aa_policy: " << message << "\n";
    }
    return condition;
}

WindowMetrics metrics_at(float dpi_scale) {
    WindowMetrics metrics;
    metrics.dpi_scale = dpi_scale;
    return metrics;
}

} // namespace

int main() {
    bool ok = true;

    // The default window (1x, unknown pixel geometry, opaque, landscape) resolves the policy the
    // design describes for 1.0: light hinting, subpixel positioning, screen-space edges, clip
    // AA.
    const AaPolicy defaults = window_aa_policy(metrics_at(1.0f), WindowAaOptions{});
    ok = expect(defaults.render_scale == 1.0f, "the default window resolves scale 1.0") && ok;
    ok = expect(defaults.hinting == TextHinting::Light,
                "1.0 selects light hinting, not the font's own hints") &&
         ok;
    ok = expect(defaults.subpixel_position, "1.0 positions glyph origins at a subpixel") && ok;
    ok = expect(defaults.text_mode == TextAaMode::Grayscale, "text stays grayscale") && ok;
    ok = expect(defaults.edge_mode == EdgeMode::ScreenSpace && defaults.clip_aa,
                "AA on selects screen-space edges and clip AA") &&
         ok;
    ok = expect(defaults.msaa_samples == 1, "MSAA stays off by default") && ok;

    // The scale ladder and hinting by scale (sections 3.2 and 3.3).
    const AaPolicy at_two = window_aa_policy(metrics_at(2.0f), WindowAaOptions{});
    ok = expect(at_two.render_scale == 2.0f, "2.0 resolves the 2.0 step") && ok;
    ok = expect(at_two.hinting == TextHinting::None, "2.0 turns hinting off") && ok;
    ok = expect(at_two.subpixel_position, "2.0 turns subpixel positioning on") && ok;
    const AaPolicy at_112 = window_aa_policy(metrics_at(1.1667f), WindowAaOptions{});
    ok = expect(at_112.render_scale == 1.25f, "112 dpi quantizes to the 1.25 step") && ok;
    ok = expect(at_112.hinting == TextHinting::Light && at_112.subpixel_position,
                "1.25 keeps light hinting and turns subpixel positioning on") &&
         ok;
    const AaPolicy invalid =
        window_aa_policy(metrics_at(std::numeric_limits<float>::quiet_NaN()), WindowAaOptions{});
    ok = expect(invalid.render_scale == 1.0f, "an invalid device scale falls back to 1.0") && ok;
    const AaPolicy tiny = window_aa_policy(metrics_at(0.5f), WindowAaOptions{});
    ok = expect(tiny.render_scale == 1.0f, "a scale below the ladder floor clamps to 1.0") && ok;

    // The facts are copied verbatim. LCD needs the device's per-channel pipeline on top of a known
    // stripe, so the window passes `lcd_supported` and the pixel geometry alone is not enough.
    WindowMetrics facts_source;
    facts_source.dpi_scale = 1.5f;
    facts_source.pixel_geometry = PixelGeometry::BgrHorizontal;
    facts_source.transparent = true;
    facts_source.orientation = Orientation::Portrait;
    const tessera::ui::scale::AaFacts facts = window_aa_facts(facts_source);
    ok =
        expect(facts.render_scale == 1.5f && facts.pixel_geometry == PixelGeometry::BgrHorizontal &&
                   facts.transparent_window && facts.orientation == Orientation::Portrait,
               "the metrics map onto the resolver facts verbatim") &&
        ok;
    WindowMetrics striped = metrics_at(1.5f);
    striped.pixel_geometry = PixelGeometry::RgbHorizontal;
    const AaPolicy no_pipeline = window_aa_policy(striped, WindowAaOptions{});
    ok = expect(no_pipeline.text_mode == TextAaMode::Grayscale,
                "a device without the LCD pipeline resolves grayscale on a known stripe") &&
         ok;
    WindowAaOptions with_lcd;
    with_lcd.lcd_supported = true;
    const AaPolicy lcd = window_aa_policy(striped, with_lcd);
    ok = expect(lcd.text_mode == TextAaMode::LcdWhenEligible,
                "a known stripe plus the LCD pipeline resolves LCD") &&
         ok;
    ok = expect(window_aa_policy(metrics_at(1.5f), with_lcd).text_mode == TextAaMode::Grayscale,
                "an unknown panel geometry stays grayscale even with the pipeline") &&
         ok;

    // The deliberate switch and the two overriding cases: all three pin the golden policy, even on
    // a 2x display with a known stripe.
    WindowMetrics two_x = metrics_at(2.0f);
    two_x.pixel_geometry = PixelGeometry::RgbHorizontal;
    WindowAaOptions switched_off;
    switched_off.display_aware_aa = false;
    ok = expect(window_aa_policy(two_x, switched_off) == golden_aa_policy(),
                "the switch off pins the golden policy") &&
         ok;
    WindowAaOptions aa_disabled;
    aa_disabled.aa_enabled = false;
    ok = expect(window_aa_policy(two_x, aa_disabled) == golden_aa_policy(),
                "an AA-off renderer pins the golden policy") &&
         ok;
    WindowAaOptions golden;
    golden.golden_mode = true;
    ok = expect(window_aa_policy(two_x, golden) == golden_aa_policy(),
                "the renderer's golden mode pins the golden policy") &&
         ok;
    ok = expect(golden_aa_policy().hinting == TextHinting::Font,
                "the golden policy is the font's own hinting, i.e. the pre-switch behavior") &&
         ok;

    // The power preset and the MSAA rule (the resolver tests pin the rule itself; this proves the
    // window maps the inputs onto it).
    WindowAaOptions four_x;
    four_x.msaa_samples = 4;
    ok = expect(window_aa_policy(metrics_at(1.0f), four_x).msaa_samples == 4,
                "4x MSAA is requested below the scale ceiling") &&
         ok;
    WindowAaOptions power_save;
    power_save.msaa_samples = 4;
    power_save.reduced_quality = true;
    ok = expect(window_aa_policy(metrics_at(1.0f), power_save).msaa_samples == 1,
                "the power preset forces one sample") &&
         ok;
    ok = expect(window_aa_policy(metrics_at(2.0f), four_x).msaa_samples == 1,
                "4x MSAA is not selected at the scale ceiling") &&
         ok;
    WindowAaOptions two_samples;
    two_samples.msaa_samples = 2;
    ok = expect(window_aa_policy(metrics_at(1.0f), two_samples).msaa_samples == 1,
                "an unsupported sample count resolves to one") &&
         ok;

    if (!ok) {
        std::cerr << "window_aa_policy: FAILED\n";
        return 1;
    }
    std::cout << "window_aa_policy: ok\n";
    return 0;
}
