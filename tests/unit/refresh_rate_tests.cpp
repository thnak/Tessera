// SPDX-License-Identifier: MIT
//
// The display-refresh source behind the adaptive present cap (docs/specs/runtime-config-v1.md
// sections 5.2 and 6.2; docs/design/performance.md section 6.1).
//
// The platform calls stay thin: Windows reads the window monitor's current DEVMODE and X11 reads
// the active RandR CRTC mode, but the meaning of the number - is it usable, what is "unknown",
// and which value reaches WindowOptions::refresh_hz - is the pure `tessera::hal` layer exercised
// here. The last case closes the loop: a known refresh makes the step-3 divisor path activate
// through window_adaptive_quality_policy, an unknown one leaves the cap at maxFps, and an explicit
// option wins over the platform report. No window, display, GPU or clock is involved.

#include "present_cap.hpp"
#include "tessera/hal/refresh_rate.hpp"
#include "tessera/runtime/config.hpp"
#include "window_config.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using tessera::app::window_adaptive_quality_policy;
using tessera::hal::kMaxDisplayRefreshHz;
using tessera::hal::kMinDisplayRefreshHz;
using tessera::hal::kUnknownRefreshHz;
using tessera::hal::refresh_hz_from_display_mode;
using tessera::hal::resolve_display_refresh_hz;
using tessera::hal::sanitize_display_refresh_hz;
using tessera::runtime::adaptive_present_cap_hz;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "refresh_rate: " << message << "\n";
    }
    return condition;
}

bool near(double value, double expected, double tolerance = 1.0e-9) {
    return std::abs(value - expected) <= tolerance;
}

// --- 1. Validation table: only a finite, sane rate survives --------------------------------

bool sanitize_table() {
    bool ok = true;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();

    // The documented "unknown" inputs: zero, negative, non-finite, and anything outside the sane
    // bounds (a sentinel or a corrupt value is never guessed into a rate).
    ok = expect(sanitize_display_refresh_hz(0.0) == kUnknownRefreshHz, "0 is unknown") && ok;
    ok =
        expect(sanitize_display_refresh_hz(-1.0) == kUnknownRefreshHz, "negative is unknown") && ok;
    ok = expect(sanitize_display_refresh_hz(-59.94) == kUnknownRefreshHz,
                "negative fractional is unknown") &&
         ok;
    ok = expect(sanitize_display_refresh_hz(nan) == kUnknownRefreshHz, "NaN is unknown") && ok;
    ok =
        expect(sanitize_display_refresh_hz(infinity) == kUnknownRefreshHz, "infinity is unknown") &&
        ok;
    ok = expect(sanitize_display_refresh_hz(-infinity) == kUnknownRefreshHz,
                "negative infinity is unknown") &&
         ok;
    ok = expect(sanitize_display_refresh_hz(0.5) == kUnknownRefreshHz,
                "below the lower bound is unknown") &&
         ok;
    ok = expect(sanitize_display_refresh_hz(kMaxDisplayRefreshHz + 0.5) == kUnknownRefreshHz,
                "above the upper bound is unknown") &&
         ok;
    ok = expect(sanitize_display_refresh_hz(5000.0) == kUnknownRefreshHz, "huge is unknown") && ok;
    ok = expect(sanitize_display_refresh_hz(1.0e9) == kUnknownRefreshHz,
                "absurdly huge is unknown") &&
         ok;

    // Real rates pass through unchanged, including the fractional ones displays actually report.
    ok = expect(near(sanitize_display_refresh_hz(kMinDisplayRefreshHz), 1.0), "1 Hz passes") && ok;
    ok = expect(near(sanitize_display_refresh_hz(59.94), 59.94), "59.94 Hz passes") && ok;
    ok = expect(near(sanitize_display_refresh_hz(60.0), 60.0), "60 Hz passes") && ok;
    ok = expect(near(sanitize_display_refresh_hz(144.0), 144.0), "144 Hz passes") && ok;
    ok =
        expect(near(sanitize_display_refresh_hz(kMaxDisplayRefreshHz), 1000.0), "1000 Hz passes") &&
        ok;
    return ok;
}

// --- 2. Explicit option wins; unknown stays unknown ---------------------------------------

bool resolve_rules() {
    bool ok = true;
    // An explicit non-zero option is the caller's decision: it wins over any platform report.
    ok = expect(near(resolve_display_refresh_hz(60.0, 144.0), 60.0), "explicit 60 beats 144") && ok;
    ok = expect(near(resolve_display_refresh_hz(59.94, 60.0), 59.94),
                "fractional explicit value is not rounded") &&
         ok;
    // An unset option (0) takes the sanitized platform report.
    ok = expect(near(resolve_display_refresh_hz(0.0, 144.0), 144.0),
                "unset takes the platform report") &&
         ok;
    // Unknown stays unknown; an unusable report never turns into a guessed rate.
    ok = expect(resolve_display_refresh_hz(0.0, 0.0) == kUnknownRefreshHz,
                "unknown stays unknown") &&
         ok;
    ok = expect(resolve_display_refresh_hz(0.0, std::numeric_limits<double>::quiet_NaN()) ==
                    kUnknownRefreshHz,
                "a NaN report stays unknown") &&
         ok;
    ok = expect(resolve_display_refresh_hz(0.0, 5000.0) == kUnknownRefreshHz,
                "an out-of-range report stays unknown") &&
         ok;
    // A negative/non-finite explicit value is not a decision (the window host rejects it earlier);
    // the resolver still behaves as "unset" instead of producing a rate the pacer would reject.
    ok = expect(near(resolve_display_refresh_hz(-1.0, 60.0), 60.0),
                "an invalid explicit value falls back to the report") &&
         ok;
    return ok;
}

// --- 3. X11/RandR mode math ---------------------------------------------------------------

bool mode_math() {
    bool ok = true;
    // 1920x1080 at 60 Hz: 148.5 MHz / (2200 * 1125). 59.94 uses the same totals with the pixel
    // clock the mode actually carries.
    ok = expect(near(refresh_hz_from_display_mode(148500000, 2200, 1125), 60.0),
                "1080p60 mode math") &&
         ok;
    ok = expect(near(refresh_hz_from_display_mode(148351500, 2200, 1125), 59.94),
                "1080p59.94 mode math") &&
         ok;
    ok = expect(near(refresh_hz_from_display_mode(356400000, 2200, 1125), 144.0),
                "144 Hz mode math") &&
         ok;
    // A synthetic mode with no timing (Xvfb) is unknown, not a divide-by-zero and not a guess.
    ok = expect(refresh_hz_from_display_mode(0, 2200, 1125) == kUnknownRefreshHz,
                "zero dot clock is unknown") &&
         ok;
    ok = expect(refresh_hz_from_display_mode(148500000, 0, 1125) == kUnknownRefreshHz,
                "zero htotal is unknown") &&
         ok;
    ok = expect(refresh_hz_from_display_mode(148500000, 2200, 0) == kUnknownRefreshHz,
                "zero vtotal is unknown") &&
         ok;
    // The mode math runs through the same sanity bound: a nonsense clock yields unknown.
    ok = expect(refresh_hz_from_display_mode(1000, 1000, 1000) == kUnknownRefreshHz,
                "sub-1 Hz mode is unknown") &&
         ok;
    ok = expect(refresh_hz_from_display_mode(1'000'000'000'000ULL, 100, 100) == kUnknownRefreshHz,
                "a huge mode rate is unknown") &&
         ok;
    return ok;
}

// --- 4. Present-cap hand-off through the policy layer -------------------------------------

bool divisor_handoff() {
    bool ok = true;
    tessera::RuntimeConfig runtime;

    // Known refresh, no cap: the policy carries the divisor base and step 1 is refresh/2. This is
    // the path that was dead when the HAL could not report a rate.
    const double known_refresh = resolve_display_refresh_hz(0.0, 144.0);
    const tessera::app::AdaptiveQualityPolicy known =
        window_adaptive_quality_policy(runtime, known_refresh);
    ok = expect(near(known.refresh_hz, 144.0), "known refresh reaches the policy") && ok;
    ok = expect(known.max_fps == 0.0, "uncapped config keeps maxFps 0") && ok;
    ok = expect(near(adaptive_present_cap_hz(known.refresh_hz, known.max_fps, 0), 0.0),
                "step 0 stays uncapped with a known refresh") &&
         ok;
    ok = expect(near(adaptive_present_cap_hz(known.refresh_hz, known.max_fps, 1), 72.0),
                "step 1 activates the 144 Hz divisor") &&
         ok;
    ok = expect(near(adaptive_present_cap_hz(known.refresh_hz, known.max_fps, 2), 48.0),
                "step 2 is the next 144 Hz divisor") &&
         ok;

    // Unknown refresh with a configured cap: no divisor exists, so the cap stays at maxFps.
    tessera::RuntimeConfig capped;
    capped.max_fps = 30;
    const double unknown_refresh = resolve_display_refresh_hz(0.0, 0.0);
    const tessera::app::AdaptiveQualityPolicy unknown =
        window_adaptive_quality_policy(capped, unknown_refresh);
    ok =
        expect(unknown.refresh_hz == kUnknownRefreshHz, "unknown refresh reaches the policy") && ok;
    ok = expect(near(unknown.max_fps, 30.0), "maxFps reaches the policy") && ok;
    for (const std::uint32_t step : {0U, 1U, 3U}) {
        ok = expect(near(adaptive_present_cap_hz(unknown.refresh_hz, unknown.max_fps, step), 30.0),
                    "no divisor with an unknown refresh: the cap stays at maxFps") &&
             ok;
    }

    // An explicit option wins over the platform report through the same hand-off: 60, not 144.
    capped.max_fps = 0;
    const double explicit_refresh = resolve_display_refresh_hz(60.0, 144.0);
    const tessera::app::AdaptiveQualityPolicy explicit_policy =
        window_adaptive_quality_policy(capped, explicit_refresh);
    ok = expect(near(explicit_policy.refresh_hz, 60.0), "explicit option reaches the policy") && ok;
    ok =
        expect(near(adaptive_present_cap_hz(explicit_policy.refresh_hz, explicit_policy.max_fps, 1),
                    30.0),
               "step 1 uses the explicit rate's divisor, not the report") &&
        ok;

    // A non-finite or out-of-range report is unknown, never a divisor base.
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(), 5000.0, -60.0}) {
        const tessera::app::AdaptiveQualityPolicy sanitized =
            window_adaptive_quality_policy(capped, resolve_display_refresh_hz(0.0, bad));
        ok = expect(sanitized.refresh_hz == kUnknownRefreshHz,
                    "an unusable report never becomes a divisor base") &&
             ok;
        ok = expect(near(adaptive_present_cap_hz(sanitized.refresh_hz, sanitized.max_fps, 1), 0.0),
                    "an unusable report keeps the cap at maxFps") &&
             ok;
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = sanitize_table() && ok;
    ok = resolve_rules() && ok;
    ok = mode_math() && ok;
    ok = divisor_handoff() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "refresh_rate: ok\n";
    return 0;
}
