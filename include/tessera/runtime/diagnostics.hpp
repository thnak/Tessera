// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/runtime/frame_counters.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::runtime {

// The frame-diagnostics aggregate: three unrelated subjects that used to share one counter bag
// (docs/architecture/god-type-audit-v1.md section 3.9). Each subject now owns its own state and
// logic in `frame_counters.hpp`, and this class forwards so the existing readers
// (`AppHost::frame_diagnostics()`) see one object with the same accessor names as before.
//
// This is the *frame* subset of diagnostics. The general logger and the metric registry live in
// `tessera/diagnostics`.
class FrameDiagnostics {
public:
    void record(FrameDiagnosticsSample sample) {
        counters_.record(sample);
    }
    void record_adaptive_quality(const AdaptiveQualitySample& sample) noexcept {
        quality_.record(sample);
    }
    void record_frame_time_source(FrameTimeSource source) noexcept {
        frame_time_.record(source);
    }

    void reset() {
        counters_.reset();
        quality_.reset();
        frame_time_.reset();
    }

    // Frame counters.
    [[nodiscard]] const FrameDiagnosticsSample& last() const {
        return counters_.last();
    }
    [[nodiscard]] std::uint64_t frames() const {
        return counters_.frames();
    }
    [[nodiscard]] std::uint64_t total_steps() const {
        return counters_.total_steps();
    }
    [[nodiscard]] std::uint64_t total_completions() const {
        return counters_.total_completions();
    }
    [[nodiscard]] std::uint64_t total_damage_rects() const {
        return counters_.total_damage_rects();
    }
    [[nodiscard]] std::uint64_t total_scissors() const {
        return counters_.total_scissors();
    }
    [[nodiscard]] std::uint64_t total_animation_samples() const {
        return counters_.total_animation_samples();
    }
    [[nodiscard]] std::uint64_t full_redraw_frames() const {
        return counters_.full_redraw_frames();
    }
    [[nodiscard]] double total_glass_ms() const {
        return counters_.total_glass_ms();
    }
    [[nodiscard]] std::uint64_t total_glass_backdrops() const {
        return counters_.total_glass_backdrops();
    }
    [[nodiscard]] std::uint64_t glass_frames() const {
        return counters_.glass_frames();
    }

    // Adaptive quality (docs/design/performance.md section 6).
    [[nodiscard]] const AdaptiveQualitySample& adaptive_quality() const noexcept {
        return quality_.sample();
    }
    [[nodiscard]] std::uint64_t adaptive_quality_changes() const noexcept {
        return quality_.changes();
    }
    [[nodiscard]] std::uint64_t adaptive_quality_downgrades() const noexcept {
        return quality_.downgrades();
    }
    [[nodiscard]] std::uint64_t adaptive_present_cap_changes() const noexcept {
        return quality_.present_cap_changes();
    }
    [[nodiscard]] std::uint64_t adaptive_effect_changes() const noexcept {
        return quality_.effect_changes();
    }
    [[nodiscard]] std::uint64_t adaptive_animation_pause_changes() const noexcept {
        return quality_.animation_pause_changes();
    }

    // Frame-time source (docs/design/performance.md section 6.1).
    [[nodiscard]] FrameTimeSource frame_time_source() const noexcept {
        return frame_time_.source();
    }
    [[nodiscard]] std::uint64_t gpu_frame_time_samples() const noexcept {
        return frame_time_.gpu_samples();
    }
    [[nodiscard]] std::uint64_t cpu_frame_time_samples() const noexcept {
        return frame_time_.cpu_samples();
    }

    // The three subjects, for a reader that wants one part on its own.
    [[nodiscard]] const FrameCounters& frame_counters() const noexcept {
        return counters_;
    }
    [[nodiscard]] const AdaptiveQualityCounters& adaptive_quality_counters() const noexcept {
        return quality_;
    }
    [[nodiscard]] const FrameTimeSourceCounter& frame_time_source_counter() const noexcept {
        return frame_time_;
    }

private:
    FrameCounters counters_;
    AdaptiveQualityCounters quality_;
    FrameTimeSourceCounter frame_time_;
};

} // namespace tessera::runtime
