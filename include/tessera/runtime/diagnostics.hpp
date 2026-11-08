// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>

namespace tessera::runtime {

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

private:
    FrameDiagnosticsSample last_;
    std::uint64_t frames_ = 0;
    std::uint64_t total_steps_ = 0;
    std::uint64_t total_completions_ = 0;
    std::uint64_t total_damage_rects_ = 0;
    std::uint64_t total_scissors_ = 0;
    std::uint64_t total_animation_samples_ = 0;
    std::uint64_t full_redraw_frames_ = 0;
};

} // namespace tessera::runtime
