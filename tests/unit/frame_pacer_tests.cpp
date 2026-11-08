// SPDX-License-Identifier: MIT

#include "tessera/runtime/frame_pacer.hpp"

#include <cmath>
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

    std::cout << "frame_pacer: ok\n";
    return 0;
}
