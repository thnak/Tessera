// SPDX-License-Identifier: MIT

#pragma once

namespace tessera::runtime {

struct FramePacerConfig {
    double max_fps = 0.0;
};

struct PaceDecision {
    bool render_now = false;
    double wait_seconds = 0.0;
    bool late = false;
};

class FramePacer {
public:
    explicit FramePacer(FramePacerConfig config = {});

    [[nodiscard]] PaceDecision ready(double now_seconds);
    void present(double now_seconds);
    void reset();

    [[nodiscard]] double max_fps() const {
        return max_fps_;
    }
    [[nodiscard]] double interval_seconds() const {
        return interval_seconds_;
    }
    [[nodiscard]] bool started() const {
        return started_;
    }

private:
    double max_fps_ = 0.0;
    double interval_seconds_ = 0.0;
    double next_deadline_ = 0.0;
    bool started_ = false;
};

} // namespace tessera::runtime
