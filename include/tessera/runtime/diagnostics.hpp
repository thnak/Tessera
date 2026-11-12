// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>

namespace tessera::runtime {

// One frame's adaptive-quality state (docs/design/performance.md section 6; the policy keys are
// `adaptiveQuality`, `minRenderScale`, `renderScale` and `frameBudgetMs` in
// docs/specs/runtime-config-v1.md section 3). The controller produces it, `FrameDiagnostics` keeps
// the latest one and counts changes.
//
// `render_scale` is the scale the frame should be rendered at, inside
// [`minRenderScale`, `renderScale`]; `quality_step` is its ladder index, 0 = full quality. As of
// this slice nothing consumes it: the render target is not resized, so the value is a report, not a
// visual change (runtime-config-v1.md section 6.2). `changed` is true only on the observation that
// produced a change; `downgrade` gives that change's direction.
struct AdaptiveQualitySample {
    float render_scale = 1.0f;
    std::uint32_t quality_step = 0;
    bool changed = false;
    bool downgrade = false;

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
};

} // namespace tessera::runtime
