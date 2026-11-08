// SPDX-License-Identifier: MIT

#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/loop_bridge.hpp"

#include <iostream>
#include <string>
#include <utility>

namespace {

using tessera::runtime::FixedStepConfig;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::runtime::RuntimeLoopConfig;
using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::animation::tick_animations;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_loop: " << message << "\n";
    }
    return condition;
}

KeyframeBlock opacity_block() {
    KeyframeBlock block;
    block.name = "fade";
    block.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", "0", false, 0}}});
    block.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", "1", false, 0}}});
    return block;
}

AnimationId start(AnimationRuntime& runtime, FillMode fill, bool paused = false) {
    AnimationInput input;
    input.target = 1;
    input.keyframes = opacity_block();
    input.timing = KeyframeTiming{};
    input.timing.duration = 1.0;
    input.timing.fill = fill;
    input.paused = paused;
    return runtime.start(std::move(input));
}

RuntimeLoop loop_for(ManualClock& clock) {
    return RuntimeLoop(clock, RuntimeLoopConfig{FixedStepConfig{60.0, 250.0, 5, 1.0}, true});
}

} // namespace

int main() {
    ManualClock clock;
    RuntimeLoop loop = loop_for(clock);

    AnimationRuntime runtime;
    const AnimationId active_id = start(runtime, FillMode::None);
    auto result = tick_animations(runtime, loop, 0.5);
    if (!expect(result.keep_awake, "active keeps awake") ||
        !expect(result.finished.empty(), "active unfinished") ||
        !expect(!result.samples.empty(), "active sample") ||
        !expect(loop.should_render(), "active requests frame")) {
        return 1;
    }
    auto loop_tick = loop.tick();
    if (!expect(loop_tick.should_render, "active frame delivered") ||
        !expect(!loop.should_render(), "frame one-shot")) {
        return 2;
    }

    result = tick_animations(runtime, loop, 0.4);
    if (!expect(result.keep_awake, "active second tick") ||
        !expect(loop.should_render(), "active rearms frame")) {
        return 3;
    }
    (void)loop.tick();

    result = tick_animations(runtime, loop, 0.2);
    if (!expect(!result.keep_awake, "finish stops wake") ||
        !expect(result.finished.size() == 1 && result.finished.front() == active_id, "finish id") ||
        !expect(loop.should_render(), "finish requests final frame")) {
        return 4;
    }
    (void)loop.tick();

    result = tick_animations(runtime, loop, 0.2);
    if (!expect(!result.keep_awake, "idle after finish") ||
        !expect(result.finished.empty(), "no repeat finish") ||
        !expect(!loop.should_render(), "idle requests nothing")) {
        return 5;
    }

    AnimationRuntime paused;
    (void)start(paused, FillMode::None, true);
    result = tick_animations(paused, loop, 0.5);
    if (!expect(!result.keep_awake, "paused no wake") ||
        !expect(result.finished.empty(), "paused unfinished") ||
        !expect(!result.samples.empty(), "paused held sample") ||
        !expect(!loop.should_render(), "paused no frame")) {
        return 6;
    }
    result = tick_animations(paused, loop, 0.5);
    if (!expect(!result.keep_awake, "paused still no wake") ||
        !expect(!loop.should_render(), "paused no rearm")) {
        return 7;
    }

    AnimationRuntime fill;
    (void)start(fill, FillMode::Forwards);
    result = tick_animations(fill, loop, 0.5);
    if (!expect(result.keep_awake, "fill active wake")) {
        return 8;
    }
    (void)loop.tick();
    result = tick_animations(fill, loop, 0.6);
    if (!expect(!result.keep_awake, "fill finish no wake") ||
        !expect(result.finished.size() == 1, "fill finished") ||
        !expect(loop.should_render(), "fill finish requests frame")) {
        return 9;
    }
    (void)loop.tick();
    result = tick_animations(fill, loop, 0.2);
    if (!expect(!result.keep_awake, "settled fill no wake") ||
        !expect(!result.samples.empty(), "settled sample") ||
        !expect(!loop.should_render(), "settled no frame")) {
        return 10;
    }

    AnimationRuntime invalid;
    (void)start(invalid, FillMode::None);
    (void)tick_animations(invalid, loop, 0.5);
    (void)loop.tick();
    result = tick_animations(invalid, loop, -1.0);
    if (!expect(result.keep_awake, "invalid dt active wake") ||
        !expect(loop.should_render(), "invalid dt frame")) {
        return 11;
    }
    (void)loop.tick();

    AnimationRuntime idle;
    result = tick_animations(idle, loop, 0.1);
    if (!expect(!result.keep_awake, "idle no wake") ||
        !expect(!loop.should_render(), "idle no frame")) {
        return 12;
    }

    std::cout << "animation_loop: ok\n";
    return 0;
}
