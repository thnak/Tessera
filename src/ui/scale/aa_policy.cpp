// SPDX-License-Identifier: MIT

#include "tessera/ui/scale/aa_policy.hpp"
#include "tessera/ui/scale/render_scale.hpp"

namespace tessera::ui::scale {

namespace {

// LCD needs a known horizontal stripe in an opaque landscape window; any other fact is grayscale.
bool lcd_possible(const AaFacts& facts) noexcept {
    return hal::pixel_geometry_is_horizontal_stripe(facts.pixel_geometry) &&
           !facts.transparent_window && facts.orientation == hal::Orientation::Landscape;
}

} // namespace

AaPolicy golden_aa_policy() noexcept {
    AaPolicy policy;
    policy.render_scale = kMinRenderScale;
    policy.text_mode = TextAaMode::Grayscale;
    policy.hinting = TextHinting::Font;
    policy.subpixel_position = false;
    policy.edge_mode = EdgeMode::Off;
    policy.clip_aa = false;
    policy.msaa_samples = 1;
    return policy;
}

AaPolicy resolve_aa_policy(const AaFacts& facts, const AaConfig& config) noexcept {
    if (config.golden_mode) {
        return golden_aa_policy();
    }

    AaPolicy policy;
    policy.render_scale = quantize_render_scale(facts.render_scale);
    policy.hinting =
        policy.render_scale >= kNoHintingMinScale ? TextHinting::None : TextHinting::Light;
    policy.subpixel_position = policy.render_scale >= kSubpixelPositionMinScale;

    if (!config.aa_enabled) {
        policy.edge_mode = EdgeMode::Off;
        policy.clip_aa = false;
        policy.msaa_samples = 1;
        return policy;
    }

    policy.edge_mode = EdgeMode::ScreenSpace;
    policy.clip_aa = true;
    policy.msaa_samples = (config.msaa_samples == 4 && !config.reduced_quality) ? 4 : 1;

    const bool wants_lcd = config.text_aa == TextAaSetting::Subpixel ||
                           (config.text_aa == TextAaSetting::Auto && kAutoAllowsLcd);
    policy.text_mode = (wants_lcd && !config.reduced_quality && lcd_possible(facts))
                           ? TextAaMode::LcdWhenEligible
                           : TextAaMode::Grayscale;
    return policy;
}

} // namespace tessera::ui::scale
