// SPDX-License-Identifier: MIT
//
// The runtime-config keys the window driver consumes (docs/specs/runtime-config-v1.md section 6.2),
// plus the display-aware AA policy mapping (docs/design/display-aware-rendering.md section 6). Pure
// mapping only: no GPU, no window, no allocation. WindowHost calls it once at construction and once
// per metrics change; unit tests exercise it directly, and the pipeline test drives the mapped
// FramePipelineConfig through the paint seam.

#pragma once

#include "tessera/app/host.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/runtime/config.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/scale/aa_policy.hpp"

#include <cstdint>

namespace tessera::app {

// Which lifecycle states pause rendering. The runtime config has one key for an invisible window,
// `pauseWhenOccluded`, so it owns both lifecycle sources (occluded and minimized). Focus/blur is
// never part of this policy: a visible background window keeps painting.
struct WindowPausePolicy {
    bool pause_when_occluded = true;
    bool pause_when_minimized = true;

    // True when a lifecycle event or metrics snapshot must pause the driver.
    [[nodiscard]] bool pauses(bool occluded, bool minimized) const noexcept {
        return (occluded && pause_when_occluded) || (minimized && pause_when_minimized);
    }

    [[nodiscard]] bool operator==(const WindowPausePolicy&) const = default;
};

// The frame pipeline's config for one window. `animationInterpolation`, `interpolationEpsilonPx`
// and the two glass limits (`glassMaxBlurRadius`, `glassMaxBackdropArea`) are wired in this slice;
// every other field keeps FramePipelineConfig's default, so an unset RuntimeConfig behaves exactly
// like the hard-coded behaviour before the key existed.
[[nodiscard]] ui::frame::FramePipelineConfig window_pipeline_config(const RuntimeConfig& runtime);

// The byte cap of the offscreen layer pool (`offscreenMaxMb`, offscreen-layers.md section 5e). The
// parser bounds the key to 1..4096 MiB; a hand-built RuntimeConfig is clamped to the same range so
// a 0 can never reach the pool, where it would mean "no cap".
[[nodiscard]] std::uint64_t window_offscreen_max_bytes(const RuntimeConfig& runtime) noexcept;

// The occluded/minimized pause policy for one window. Only `pauseWhenOccluded` is wired in this
// slice; the remaining lifecycle keys stay unwired.
[[nodiscard]] WindowPausePolicy window_pause_policy(const RuntimeConfig& runtime);

// The adaptive-quality policy for one window (docs/design/performance.md section 6): the
// `adaptiveQuality`, `powerPolicy`, `renderScale`, `minRenderScale` and `frameBudgetMs` keys, plus
// the effective present rate that derives the budget when `frameBudgetMs` is 0. That rate is
// `maxFps` when it is capped, else `preferredRefreshRate` when requested, else the 60 Hz baseline
// the framework's default fixed timestep uses (the pacer still follows the real display; feeding it
// a later measured rate is a follow-up, not part of this slice).
//
// `refresh_hz` is the display refresh the platform reported (0 = unknown). It is the divisor base
// of the step-3 present cap and the ceiling the cap may never exceed; with 0 there is no divisor,
// so the cap stays at `maxFps` and the pacer keeps its documented deadline pacing.
[[nodiscard]] AdaptiveQualityPolicy window_adaptive_quality_policy(const RuntimeConfig& runtime,
                                                                   double refresh_hz = 0.0);

// The renderer inputs the AA resolver reads (docs/design/display-aware-rendering.md sections 5 and
// 6), mirrored as plain data so this mapping layer stays free of GPU headers. The window driver
// fills it from the `Batched2DConfig` it creates the renderer with, so the policy and the renderer
// can never disagree about whether AA is on.
struct WindowAaOptions {
    // The explicit switch (`WindowOptions::display_aware_aa`). Off pins `golden_aa_policy()`, which
    // is the pre-policy behaviour: turning the policy on changes default text hinting from the
    // font's own hints to light, so it is a deliberate opt-in/out, never a side effect.
    bool display_aware_aa = true;
    bool aa_enabled = true;
    // The renderer's deterministic-test mode. It wins over every other input.
    bool golden_mode = false;
    // 1 or 4; the resolver ignores any other value, exactly like `AaConfig`.
    std::uint8_t msaa_samples = 1;
    // The power policy's cheaper preset (ADR-019): grayscale text and no MSAA.
    bool reduced_quality = false;
    // The device can composite LCD text per channel (dual-source blending) and the LCD pipeline
    // exists. False resolves `Auto` text to grayscale instead of paying for a three-channel atlas
    // that no pipeline could composite.
    bool lcd_supported = false;
};

// The display facts the policy reads, copied verbatim from the window's metrics.
[[nodiscard]] ui::scale::AaFacts window_aa_facts(const hal::WindowMetrics& metrics) noexcept;

// The window's anti-aliasing policy (docs/design/display-aware-rendering.md section 6). Resolved
// from the metrics plus the renderer inputs, once per metrics change and once at creation - never
// on the frame path. Golden mode (the switch off, a renderer with AA disabled, or the renderer's
// golden flag) returns `golden_aa_policy()` regardless of the metrics, so a deterministic run never
// depends on the display.
[[nodiscard]] ui::scale::AaPolicy window_aa_policy(const hal::WindowMetrics& metrics,
                                                   const WindowAaOptions& renderer) noexcept;

// The sample count the presentation stack may use for the policy's MSAA decision (display-aware
// rendering S8, section 4): 4 only when the resolver asked for it, the run is not golden, and the
// device reports 4x support for the frame format; otherwise 1. Pure, so the capability/golden gate
// is unit-testable without a window, a GPU or a display.
[[nodiscard]] std::uint32_t presentation_sample_count(std::uint8_t policy_samples, bool golden_mode,
                                                      bool device_supports_4) noexcept;

} // namespace tessera::app
