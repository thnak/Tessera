// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>

namespace tessera::runtime {

// Which signal produced one adaptive-quality observation (docs/design/performance.md section 6.1).
// `Gpu` means the sample came from the RHI timestamp probe's completed submission interval; `Cpu`
// is the always-available fallback (also the source for the null backend and the first frame,
// because a GPU interval is only readable after its submission completed).
enum class FrameTimeSource { Cpu, Gpu };

// One frame's adaptive-quality state (docs/design/performance.md section 6; the policy keys are
// `adaptiveQuality`, `minRenderScale`, `renderScale` and `frameBudgetMs` in
// docs/specs/runtime-config-v1.md section 3). The controller produces it, `FrameDiagnostics` keeps
// the latest one and counts changes.
//
// `render_scale` is the scale the frame should be rendered at, inside
// [`minRenderScale`, `renderScale`]; `quality_step` is the ladder position, 0 = full quality. The
// ladder is ADR-019 section 8's four ordered actions: position 1 reduces optional effects
// (`effects_reduced`), positions 2..5 walk `renderScale` and the present divisor, and the last
// position pauses non-essential animations (`animations_paused`). Each flag stays set at every
// later position and clears on recovery. `present_cap_hz` is the present cap for the scale/divisor
// step (docs/design/performance.md section 6, step 3):
// the configured `maxFps` at step 0, the next refresh divisor below the previous step's effective
// rate after each downgrade, and never below kAdaptivePresentCapFloorHz. 0 means uncapped, which is
// also what an unknown display refresh leaves in place (no divisor exists, so the pacer keeps its
// deadline pacing). `changed` is true only on the observation that produced a change; `downgrade`
// gives that change's direction.
struct AdaptiveQualitySample {
    float render_scale = 1.0f;
    std::uint32_t quality_step = 0;
    bool changed = false;
    bool downgrade = false;
    double present_cap_hz = 0.0;
    bool effects_reduced = false;
    bool animations_paused = false;

    [[nodiscard]] bool operator==(const AdaptiveQualitySample&) const = default;
};

struct FrameDiagnosticsSample {
    std::uint64_t frame_index = 0;
    std::uint32_t steps = 0;
    std::size_t completions = 0;
    std::size_t pending_completions = 0;
    std::size_t damage_rects = 0;
    std::size_t redraw_scissors = 0;
    std::size_t animation_samples = 0;
    bool full_redraw = false;
};

class FrameDiagnostics {
public:
    void record(FrameDiagnosticsSample sample);
    void reset();

    // Adaptive quality (docs/design/performance.md section 6): one call per observed frame. The
    // latest sample is always kept so a reader sees the quality in force; only a change increments
    // a counter, which is what makes a downgrade visible without sampling frame time. Callers that
    // never observe a frame leave every field at its full-quality default.
    void record_adaptive_quality(const AdaptiveQualitySample& sample) noexcept;

    // Records which signal produced one adaptive-quality observation. `last_` is what the most
    // recent observation used; the two counters make a fallback explicable instead of letting a
    // stale or missing timestamp pass for a GPU number (docs/design/performance.md section 6.1).
    void record_frame_time_source(FrameTimeSource source) noexcept;

    [[nodiscard]] const FrameDiagnosticsSample& last() const {
        return last_;
    }
    [[nodiscard]] const AdaptiveQualitySample& adaptive_quality() const noexcept {
        return adaptive_quality_;
    }
    [[nodiscard]] std::uint64_t adaptive_quality_changes() const noexcept {
        return adaptive_quality_changes_;
    }
    [[nodiscard]] std::uint64_t adaptive_quality_downgrades() const noexcept {
        return adaptive_quality_downgrades_;
    }
    // Changes that actually moved the present cap (step 3). A step whose cap is already at the
    // floor changes the scale but not the cap, so this counter can lag adaptive_quality_changes().
    [[nodiscard]] std::uint64_t adaptive_present_cap_changes() const noexcept {
        return adaptive_present_cap_changes_;
    }
    // Changes that flipped `effects_reduced` / `animations_paused` (ladder ends, #147), counted in
    // both directions so a recovery is as visible as a downgrade.
    [[nodiscard]] std::uint64_t adaptive_effect_changes() const noexcept {
        return adaptive_effect_changes_;
    }
    [[nodiscard]] std::uint64_t adaptive_animation_pause_changes() const noexcept {
        return adaptive_animation_pause_changes_;
    }
    [[nodiscard]] FrameTimeSource frame_time_source() const noexcept {
        return frame_time_source_;
    }
    [[nodiscard]] std::uint64_t gpu_frame_time_samples() const noexcept {
        return gpu_frame_time_samples_;
    }
    [[nodiscard]] std::uint64_t cpu_frame_time_samples() const noexcept {
        return cpu_frame_time_samples_;
    }
    [[nodiscard]] std::uint64_t frames() const {
        return frames_;
    }
    [[nodiscard]] std::uint64_t total_steps() const {
        return total_steps_;
    }
    [[nodiscard]] std::uint64_t total_completions() const {
        return total_completions_;
    }
    [[nodiscard]] std::uint64_t total_damage_rects() const {
        return total_damage_rects_;
    }
    [[nodiscard]] std::uint64_t total_scissors() const {
        return total_scissors_;
    }
    [[nodiscard]] std::uint64_t total_animation_samples() const {
        return total_animation_samples_;
    }
    [[nodiscard]] std::uint64_t full_redraw_frames() const {
        return full_redraw_frames_;
    }

private:
    FrameDiagnosticsSample last_;
    AdaptiveQualitySample adaptive_quality_;
    std::uint64_t frames_ = 0;
    std::uint64_t total_steps_ = 0;
    std::uint64_t total_completions_ = 0;
    std::uint64_t total_damage_rects_ = 0;
    std::uint64_t total_scissors_ = 0;
    std::uint64_t total_animation_samples_ = 0;
    std::uint64_t full_redraw_frames_ = 0;
    std::uint64_t adaptive_quality_changes_ = 0;
    std::uint64_t adaptive_quality_downgrades_ = 0;
    std::uint64_t adaptive_present_cap_changes_ = 0;
    std::uint64_t adaptive_effect_changes_ = 0;
    std::uint64_t adaptive_animation_pause_changes_ = 0;
    FrameTimeSource frame_time_source_ = FrameTimeSource::Cpu;
    std::uint64_t gpu_frame_time_samples_ = 0;
    std::uint64_t cpu_frame_time_samples_ = 0;
};

} // namespace tessera::runtime
