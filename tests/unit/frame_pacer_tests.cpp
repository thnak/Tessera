// SPDX-License-Identifier: MIT

#include "tessera/runtime/frame_pacer.hpp"

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frame_pacer: " << message << "\n";
    }
    return condition;
}

bool near(double value, double expected, double tolerance = 1.0e-12) {
    return std::abs(value - expected) <= tolerance;
}

// Presents at `start`, then checks `frames` periods of `interval`: not ready a quarter interval
// early, ready (and on time) at each deadline, and presenting on the deadline keeps the cadence.
bool script_cadence(tessera::runtime::FramePacer& pacer, double start, double interval, int frames,
                    const char* message) {
    pacer.present(start);
    for (int frame = 1; frame <= frames; ++frame) {
        const double deadline = start + frame * interval;
        auto decision = pacer.ready(deadline - interval * 0.25);
        if (!expect(!decision.render_now, message) ||
            !expect(near(decision.wait_seconds, interval * 0.25), message)) {
            return false;
        }
        decision = pacer.ready(deadline);
        if (!expect(decision.render_now, message) || !expect(!decision.late, message)) {
            return false;
        }
        pacer.present(deadline);
    }
    return true;
}

} // namespace

int main() {
    using tessera::runtime::FramePacer;
    using tessera::runtime::FramePacerConfig;

    FramePacer uncapped(FramePacerConfig{0.0});
    auto decision = uncapped.ready(0.0);
    if (!expect(decision.render_now, "uncapped renders") ||
        !expect(near(decision.wait_seconds, 0.0), "uncapped no wait")) {
        return 1;
    }
    uncapped.present(0.0);
    if (!expect(uncapped.started(), "uncapped records present")) {
        return 2;
    }

    FramePacer pacer(FramePacerConfig{60.0});
    if (!expect(near(pacer.interval_seconds(), 1.0 / 60.0), "60 Hz interval")) {
        return 3;
    }

    decision = pacer.ready(0.0);
    if (!expect(decision.render_now, "first frame renders immediately")) {
        return 4;
    }
    pacer.present(0.0);

    decision = pacer.ready(0.005);
    if (!expect(!decision.render_now, "early frame waits") ||
        !expect(near(decision.wait_seconds, 1.0 / 60.0 - 0.005), "wait to deadline") ||
        !expect(!decision.late, "early frame is not late")) {
        return 5;
    }

    decision = pacer.ready(1.0 / 60.0);
    if (!expect(decision.render_now, "deadline renders") ||
        !expect(near(decision.wait_seconds, 0.0), "deadline no wait")) {
        return 6;
    }
    pacer.present(1.0 / 60.0);

    decision = pacer.ready(0.05);
    if (!expect(decision.render_now, "late frame renders") ||
        !expect(decision.late, "late frame reports lateness")) {
        return 7;
    }
    pacer.present(0.05);

    decision = pacer.ready(0.0501);
    if (!expect(!decision.render_now, "late present resets cadence") ||
        !expect(near(decision.wait_seconds, 1.0 / 60.0 - 0.0001), "post-late wait")) {
        return 8;
    }

    // A frame that finishes a little after its deadline must not stretch the period: with the
    // work taking 4 ms, deadlines stay on the 1/60 grid instead of drifting to 1/60 + 4 ms.
    pacer.reset();
    pacer.present(0.0);
    double deadline = 1.0 / 60.0;
    for (int frame = 1; frame <= 5; ++frame) {
        decision = pacer.ready(deadline);
        if (!expect(decision.render_now, "frame renders at its deadline")) {
            return 11;
        }
        pacer.present(deadline + 0.004);
        deadline += 1.0 / 60.0;
        decision = pacer.ready(deadline - 0.001);
        if (!expect(!decision.render_now && near(decision.wait_seconds, 0.001),
                    "slightly late frame keeps the cadence")) {
            return 12;
        }
    }

    pacer.reset();
    if (!expect(!pacer.started(), "reset clears start state")) {
        return 9;
    }
    decision = pacer.ready(100.0);
    if (!expect(decision.render_now, "after reset first frame renders")) {
        return 10;
    }

    bool threw = false;
    try {
        FramePacer invalid(FramePacerConfig{-1.0});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "negative max_fps rejected")) {
        return 11;
    }

    threw = false;
    try {
        (void)pacer.ready(std::numeric_limits<double>::quiet_NaN());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "NaN time rejected")) {
        return 12;
    }

    // Display refresh known with no cap: follow the display cadence (60 Hz -> 16.667 ms).
    FramePacer display60(FramePacerConfig{0.0, 60.0, false});
    if (!expect(near(display60.interval_seconds(), 1.0 / 60.0), "60 Hz display interval") ||
        !expect(near(display60.refresh_hz(), 60.0), "refresh accessor") ||
        !expect(!display60.vrr_active(), "VRR off by default")) {
        return 13;
    }
    if (!script_cadence(display60, 0.0, 1.0 / 60.0, 4, "60 Hz display cadence")) {
        return 14;
    }

    // Refresh divisor: 144 Hz with max_fps 72 presents every second refresh (2/144 = 13.888 ms).
    // For an exact divisor the interval equals 1/max_fps, but the divisor path keeps the deadline
    // on the display grid instead of free-running from the cap.
    FramePacer divisor(FramePacerConfig{72.0, 144.0, false});
    if (!expect(near(divisor.interval_seconds(), 2.0 / 144.0), "144/72 divisor interval")) {
        return 15;
    }
    if (!script_cadence(divisor, 0.0, 2.0 / 144.0, 3, "144/72 divisor cadence")) {
        return 16;
    }

    // A cap that is not a refresh divisor falls back to 1/max_fps.
    FramePacer not_divisor(FramePacerConfig{50.0, 144.0, false});
    if (!expect(near(not_divisor.interval_seconds(), 1.0 / 50.0), "non-divisor cap interval")) {
        return 17;
    }

    // 60 Hz with max_fps 30: second refresh, 2/60 = 33.333 ms.
    FramePacer half_rate(FramePacerConfig{30.0, 60.0, false});
    if (!expect(near(half_rate.interval_seconds(), 2.0 / 60.0), "60/30 divisor interval")) {
        return 19;
    }
    if (!script_cadence(half_rate, 0.0, 2.0 / 60.0, 3, "60/30 divisor cadence")) {
        return 20;
    }

    // VRR: the display follows the content rate, so an uncapped pacer schedules no wait at all.
    FramePacer vrr(FramePacerConfig{0.0, 144.0, true});
    if (!expect(near(vrr.interval_seconds(), 0.0), "VRR uncapped interval") ||
        !expect(vrr.vrr_active(), "VRR accessor")) {
        return 21;
    }
    for (const double now : {0.0, 0.0001, 5.0, 100.0}) {
        decision = vrr.ready(now);
        if (!expect(decision.render_now, "VRR uncapped renders immediately") ||
            !expect(near(decision.wait_seconds, 0.0), "VRR uncapped no wait") ||
            !expect(!decision.late, "VRR uncapped not late")) {
            return 22;
        }
        vrr.present(now);
    }

    FramePacer vrr_capped(FramePacerConfig{60.0, 144.0, true});
    if (!expect(near(vrr_capped.interval_seconds(), 1.0 / 60.0), "VRR capped interval")) {
        return 24;
    }
    if (!script_cadence(vrr_capped, 0.0, 1.0 / 60.0, 3, "VRR cap cadence")) {
        return 25;
    }

    // A scripted mid-run policy change resyncs the next deadline to one new interval after the
    // change instant: no burst of catch-up renders, no stall at an old deadline.
    FramePacer scripted(FramePacerConfig{60.0, 60.0, false});
    if (!script_cadence(scripted, 0.0, 1.0 / 60.0, 5, "scripted 60 Hz start")) {
        return 26;
    }
    const double change = 5.0 / 60.0;
    scripted.update(FramePacerConfig{72.0, 144.0, false}, change);
    if (!expect(near(scripted.interval_seconds(), 2.0 / 144.0),
                "scripted update divisor interval")) {
        return 27;
    }
    decision = scripted.ready(change);
    if (!expect(!decision.render_now, "policy change does not burst a frame") ||
        !expect(near(decision.wait_seconds, 2.0 / 144.0), "policy change waits one new interval") ||
        !expect(!decision.late, "policy change is not late")) {
        return 28;
    }
    decision = scripted.ready(change + 2.0 / 144.0);
    if (!expect(decision.render_now, "policy change frame renders at its new deadline")) {
        return 29;
    }
    scripted.present(change + 2.0 / 144.0);

    // Third change: the cap is removed on a 144 Hz display -> follow the display cadence.
    const double refresh_change = change + 2.0 / 144.0;
    scripted.update(FramePacerConfig{0.0, 144.0, false}, refresh_change);
    if (!expect(near(scripted.interval_seconds(), 1.0 / 144.0), "scripted refresh-only interval")) {
        return 30;
    }
    decision = scripted.ready(refresh_change);
    if (!expect(!decision.render_now, "refresh-only change waits") ||
        !expect(near(decision.wait_seconds, 1.0 / 144.0),
                "refresh-only change waits one new interval")) {
        return 31;
    }
    scripted.present(refresh_change + 1.0 / 144.0);

    // Fourth change: unknown refresh and no cap -> uncapped, effective on the next call.
    const double uncap = refresh_change + 1.0 / 144.0;
    scripted.update(FramePacerConfig{0.0, 0.0, false}, uncap);
    if (!expect(near(scripted.interval_seconds(), 0.0), "scripted uncapped interval")) {
        return 32;
    }
    decision = scripted.ready(uncap);
    if (!expect(decision.render_now, "uncapped takes effect on the next call") ||
        !expect(near(decision.wait_seconds, 0.0), "uncapped next call has no wait")) {
        return 33;
    }

    // Regression: a cap-only change while the refresh stays unknown keeps the old cadence rule.
    FramePacer cap_only(FramePacerConfig{60.0});
    cap_only.present(0.0);
    const double cap_change = 1.0 / 120.0;
    cap_only.update(FramePacerConfig{30.0}, cap_change);
    if (!expect(near(cap_only.interval_seconds(), 1.0 / 30.0), "cap-only change interval") ||
        !expect(near(cap_only.refresh_hz(), 0.0), "cap-only unknown refresh") ||
        !expect(!cap_only.vrr_active(), "cap-only VRR stays off")) {
        return 34;
    }
    decision = cap_only.ready(cap_change);
    if (!expect(!decision.render_now, "cap-only change waits") ||
        !expect(near(decision.wait_seconds, 1.0 / 30.0), "cap-only new interval wait")) {
        return 35;
    }
    decision = cap_only.ready(cap_change + 1.0 / 30.0);
    if (!expect(decision.render_now, "cap-only change renders at the new deadline")) {
        return 36;
    }

    // A change before the first present only updates the policy; the first frame still renders
    // immediately and the new interval starts from it.
    FramePacer prestart(FramePacerConfig{60.0});
    prestart.update(FramePacerConfig{72.0, 144.0}, 0.0);
    if (!expect(near(prestart.interval_seconds(), 2.0 / 144.0), "update before start interval") ||
        !expect(prestart.ready(0.0).render_now, "update before start renders first frame")) {
        return 37;
    }

    threw = false;
    try {
        pacer.update(FramePacerConfig{60.0, -1.0}, 0.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "negative refresh_hz rejected")) {
        return 38;
    }

    std::cout << "frame_pacer: ok\n";
    return 0;
}
