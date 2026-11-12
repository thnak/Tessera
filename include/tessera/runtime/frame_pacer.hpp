// SPDX-License-Identifier: MIT

#pragma once

namespace tessera::runtime {

struct FramePacerConfig {
    // Present-rate cap: 0 = uncapped (the display cadence applies when `refresh_hz` is known).
    // Kept first so existing `FramePacerConfig{max_fps}` aggregate initializations keep working.
    double max_fps = 0.0;
    // Display refresh in Hz; 0 = unknown.
    double refresh_hz = 0.0;
    // True when the display follows the content rate (VRR): the display imposes no cadence and
    // only a non-zero `max_fps` caps the present rate.
    bool vrr_active = false;
};

struct PaceDecision {
    bool render_now = false;
    double wait_seconds = 0.0;
    bool late = false;
};

class FramePacer {
public:
    explicit FramePacer(FramePacerConfig config = {});

    // Replaces the pacing policy. `now_seconds` is the instant the change takes effect; a started
    // pacer resyncs its next deadline to `now_seconds + interval`, so a rate change neither replays
    // missed slots nor waits out the old cadence. A change that leaves the derived interval
    // unchanged keeps the running deadline. Before the first present there is no deadline to
    // resync; the time is still validated.
    void update(FramePacerConfig config, double now_seconds);

    [[nodiscard]] PaceDecision ready(double now_seconds);
    void present(double now_seconds);
    void reset();

    [[nodiscard]] double max_fps() const {
        return max_fps_;
    }
    [[nodiscard]] double refresh_hz() const {
        return refresh_hz_;
    }
    [[nodiscard]] bool vrr_active() const {
        return vrr_active_;
    }
    [[nodiscard]] double interval_seconds() const {
        return interval_seconds_;
    }
    [[nodiscard]] bool started() const {
        return started_;
    }

private:
    double max_fps_ = 0.0;
    double refresh_hz_ = 0.0;
    bool vrr_active_ = false;
    double interval_seconds_ = 0.0;
    double next_deadline_ = 0.0;
    bool started_ = false;
};

} // namespace tessera::runtime
