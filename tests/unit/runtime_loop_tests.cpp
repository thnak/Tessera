// SPDX-License-Identifier: MIT

#include "tessera/runtime/runtime_loop.hpp"

#include <cmath>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "runtime_loop: " << message << "\n";
    }
    return condition;
}

bool near(double value, double expected, double tolerance = 1.0e-9) {
    return std::abs(value - expected) <= tolerance;
}

} // namespace

int main() {
    using tessera::runtime::FixedStepConfig;
    using tessera::runtime::ManualClock;
    using tessera::runtime::RuntimeLoop;
    using tessera::runtime::RuntimeLoopConfig;

    ManualClock clock;
    RuntimeLoop loop(clock, RuntimeLoopConfig{FixedStepConfig{60.0, 250.0, 5, 1.0}, true});

    auto tick = loop.tick();
    if (!expect(tick.steps == 0, "first tick has no delta") ||
        !expect(!tick.should_render, "first on-demand tick is idle")) {
        return 1;
    }

    clock.advance(1.0 / 60.0);
    tick = loop.tick();
    if (!expect(tick.steps == 1, "clock delta advances one step") ||
        !expect(!tick.should_render, "simulation step alone does not render")) {
        return 2;
    }

    loop.mark_render_dirty();
    tick = loop.tick();
    if (!expect(tick.should_render, "dirty render request")) {
        return 3;
    }
    tick = loop.tick();
    if (!expect(tick.should_render, "dirty render request persists")) {
        return 4;
    }

    loop.clear_render_dirty();
    tick = loop.tick();
    if (!expect(!tick.should_render, "cleared dirty request")) {
        return 5;
    }

    loop.request_frame();
    tick = loop.tick();
    if (!expect(tick.should_render, "explicit frame request")) {
        return 6;
    }
    tick = loop.tick();
    if (!expect(!tick.should_render, "explicit frame request is one-shot")) {
        return 7;
    }

    loop.set_continuous(true);
    tick = loop.tick();
    if (!expect(tick.should_render, "continuous mode renders")) {
        return 8;
    }

    loop.set_continuous(false);
    loop.set_render_on_demand(false);
    tick = loop.tick();
    if (!expect(tick.should_render, "non-on-demand mode renders")) {
        return 9;
    }

    loop.set_render_on_demand(true);
    loop.mark_render_dirty();
    loop.reset();
    if (!expect(!loop.should_render(), "reset clears wake state")) {
        return 10;
    }

    tick = loop.tick();
    if (!expect(tick.steps == 0, "first tick after reset only anchors the clock")) {
        return 11;
    }
    clock.advance(1.0);
    tick = loop.tick();
    if (!expect(tick.steps == 5, "reset re-anchors the clock") ||
        !expect(tick.dropped_backlog, "long pause drops backlog") ||
        !expect(near(tick.alpha, 1.0), "backlog alpha is clamped")) {
        return 12;
    }

    std::cout << "runtime_loop: ok\n";
    return 0;
}
