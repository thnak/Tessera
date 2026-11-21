// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace tessera::runtime {

// Which signal produced one adaptive-quality observation (docs/design/performance.md section 6.1).
// `Gpu` means the sample came from the RHI timestamp probe's completed submission interval; `Cpu`
// is the always-available fallback (also the source for the null backend and the first frame,
// because a GPU interval is only readable after its submission completed).
enum class FrameTimeSource { Cpu, Gpu };

// One frame's adaptive-quality state (docs/design/performance.md section 6; the policy keys are
// `adaptiveQuality`, `minRenderScale`, `renderScale` and `frameBudgetMs` in
// docs/specs/runtime-config-v1.md section 3). The controller produces it, `AdaptiveQualityCounters`
// keeps the latest one and counts changes.
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
// Members are ordered by alignment, widest first (issue #731): the `double` used to sit after two
// bools, which cost 6 bytes of padding before it and 6 more at the tail (32 bytes for 20 of data).
// This order is 24.
struct AdaptiveQualitySample {
    double present_cap_hz = 0.0;
    float render_scale = 1.0f;
    std::uint32_t quality_step = 0;
    bool changed = false;
    bool downgrade = false;
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
    // Glass (docs/specs/glass-v1.md section 8): time this frame spent on glass and how many
    // backdrop blur draws it recorded. A slow frame with glass active is attributable instead of
    // looking like a generic render miss; neither value changes what is painted.
    double glass_ms = 0.0;
    std::size_t glass_backdrop_count = 0;
};

// Per-frame counters and their cumulative totals: what the frame loop itself did. One of the three
// subjects that used to share `FrameDiagnostics`'s single counter bag
// (docs/architecture/god-type-audit-v1.md section 3.9).
class FrameCounters {
public:
    void record(FrameDiagnosticsSample sample) {
        ++frames_;
        sample.frame_index = frames_;
        last_ = sample;

        total_steps_ += sample.steps;
        total_completions_ += sample.completions;
        total_damage_rects_ += sample.damage_rects;
        total_scissors_ += sample.redraw_scissors;
        total_animation_samples_ += sample.animation_samples;
        total_glass_ms_ += sample.glass_ms;
        total_glass_backdrops_ += sample.glass_backdrop_count;
        if (sample.glass_ms > 0.0 || sample.glass_backdrop_count > 0) {
            ++glass_frames_;
        }
        if (sample.full_redraw) {
            ++full_redraw_frames_;
        }
    }

    void reset() {
        *this = FrameCounters{};
    }

    [[nodiscard]] const FrameDiagnosticsSample& last() const {
        return last_;
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
    // Cumulative glass cost and usage: total time reported in `glass_ms`, total backdrop blur
    // draws, and how many recorded frames had any glass work.
    [[nodiscard]] double total_glass_ms() const {
        return total_glass_ms_;
    }
    [[nodiscard]] std::uint64_t total_glass_backdrops() const {
        return total_glass_backdrops_;
    }
    [[nodiscard]] std::uint64_t glass_frames() const {
        return glass_frames_;
    }

private:
    FrameDiagnosticsSample last_;
    std::uint64_t frames_ = 0;
    std::uint64_t total_steps_ = 0;
    std::uint64_t total_completions_ = 0;
    std::uint64_t total_damage_rects_ = 0;
    std::uint64_t total_scissors_ = 0;
    std::uint64_t total_animation_samples_ = 0;
    std::uint64_t full_redraw_frames_ = 0;
    double total_glass_ms_ = 0.0;
    std::uint64_t total_glass_backdrops_ = 0;
    std::uint64_t glass_frames_ = 0;
};

// Adaptive-quality state and the counters that make a downgrade visible without sampling frame time
// (docs/design/performance.md section 6). One call per observed frame: the latest sample is always
// kept so a reader sees the quality in force; only a change increments a counter. Callers that
// never observe a frame leave every field at its full-quality default.
class AdaptiveQualityCounters {
public:
    void record(const AdaptiveQualitySample& sample) noexcept {
        // The cap is part of the sample, so a change is visible even when only the cap moved (a
        // step whose scale was already at the floor). Comparing against the previous sample keeps
        // the counter honest without the caller having to report the cap separately.
        const bool cap_changed = sample.present_cap_hz != sample_.present_cap_hz;
        const bool effects_changed = sample.effects_reduced != sample_.effects_reduced;
        const bool pause_changed = sample.animations_paused != sample_.animations_paused;
        sample_ = sample;
        if (!sample.changed) {
            return;
        }
        ++changes_;
        if (sample.downgrade) {
            ++downgrades_;
        }
        if (cap_changed) {
            ++present_cap_changes_;
        }
        if (effects_changed) {
            ++effect_changes_;
        }
        if (pause_changed) {
            ++animation_pause_changes_;
        }
    }

    void reset() noexcept {
        *this = AdaptiveQualityCounters{};
    }

    [[nodiscard]] const AdaptiveQualitySample& sample() const noexcept {
        return sample_;
    }
    [[nodiscard]] std::uint64_t changes() const noexcept {
        return changes_;
    }
    [[nodiscard]] std::uint64_t downgrades() const noexcept {
        return downgrades_;
    }
    // Changes that actually moved the present cap (step 3). A step whose cap is already at the
    // floor changes the scale but not the cap, so this counter can lag `changes()`.
    [[nodiscard]] std::uint64_t present_cap_changes() const noexcept {
        return present_cap_changes_;
    }
    // Changes that flipped `effects_reduced` / `animations_paused` (ladder ends, #147), counted in
    // both directions so a recovery is as visible as a downgrade.
    [[nodiscard]] std::uint64_t effect_changes() const noexcept {
        return effect_changes_;
    }
    [[nodiscard]] std::uint64_t animation_pause_changes() const noexcept {
        return animation_pause_changes_;
    }

private:
    AdaptiveQualitySample sample_;
    std::uint64_t changes_ = 0;
    std::uint64_t downgrades_ = 0;
    std::uint64_t present_cap_changes_ = 0;
    std::uint64_t effect_changes_ = 0;
    std::uint64_t animation_pause_changes_ = 0;
};

// Records which signal produced one adaptive-quality observation. The most recent source is what a
// reader sees; the two counters make a fallback explicable instead of letting a stale or missing
// timestamp pass for a GPU number (docs/design/performance.md section 6.1).
class FrameTimeSourceCounter {
public:
    void record(FrameTimeSource source) noexcept {
        source_ = source;
        if (source == FrameTimeSource::Gpu) {
            ++gpu_samples_;
        } else {
            ++cpu_samples_;
        }
    }

    void reset() noexcept {
        *this = FrameTimeSourceCounter{};
    }

    [[nodiscard]] FrameTimeSource source() const noexcept {
        return source_;
    }
    [[nodiscard]] std::uint64_t gpu_samples() const noexcept {
        return gpu_samples_;
    }
    [[nodiscard]] std::uint64_t cpu_samples() const noexcept {
        return cpu_samples_;
    }

private:
    FrameTimeSource source_ = FrameTimeSource::Cpu;
    std::uint64_t gpu_samples_ = 0;
    std::uint64_t cpu_samples_ = 0;
};

} // namespace tessera::runtime
