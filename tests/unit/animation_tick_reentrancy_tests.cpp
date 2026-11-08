// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/runtime.hpp"

#include <cmath>
#include <iostream>
#include <string>
#include <utility>

namespace {

using tessera::ui::Dirty;
using tessera::ui::animation::AnimationCallback;
using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::AnimationTickResult;
using tessera::ui::animation::FillMode;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_tick_reentrancy: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

KeyframeBlock block(std::string name) {
    KeyframeBlock result;
    result.name = std::move(name);
    result.steps.push_back(KeyframeStep{0.0, {Declaration{"opacity", "0", false, 0}}});
    result.steps.push_back(KeyframeStep{100.0, {Declaration{"opacity", "1", false, 0}}});
    return result;
}

AnimationId start(AnimationRuntime& runtime, std::uint64_t target, std::string name,
                  double duration, FillMode fill = FillMode::None,
                  AnimationCallback callback = {}) {
    AnimationInput input;
    input.target = target;
    input.keyframes = block(std::move(name));
    input.timing.duration = duration;
    input.timing.fill = fill;
    input.on_complete = std::move(callback);
    return runtime.start(std::move(input));
}

const tessera::ui::animation::AnimationSample* sample_for(const AnimationTickResult& result,
                                                          AnimationId id) {
    for (const auto& sample : result.samples) {
        if (sample.id == id) {
            return &sample;
        }
    }
    return nullptr;
}

const std::string* opacity_for(const tessera::ui::animation::AnimationSample* sample) {
    if (sample == nullptr) {
        return nullptr;
    }
    for (const auto& declaration : sample->declarations) {
        if (declaration.property == "opacity") {
            return &declaration.value;
        }
    }
    return nullptr;
}

bool finished_has(const AnimationTickResult& result, AnimationId id) {
    for (const AnimationId finished : result.finished) {
        if (finished == id) {
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    AnimationRuntime cleared;
    (void)start(cleared, 1, "sibling", 2.0);
    AnimationId clear_id = 0;
    clear_id = start(cleared, 1, "finish", 1.0, FillMode::Forwards, [&](AnimationId id) {
        if (!expect(id == clear_id, "clear callback id")) {
            return;
        }
        cleared.clear();
    });
    const auto clear_result = cleared.tick(1.0);
    if (!expect(finished_has(clear_result, clear_id), "clear finished") ||
        !expect(clear_result.samples.empty(), "clear drops all samples") ||
        !expect(clear_result.dirty == Dirty::None, "clear drops dirty") ||
        !expect(cleared.active() == 0 && cleared.settled() == 0, "clear state")) {
        return 1;
    }

    AnimationRuntime stopped;
    const AnimationId sibling = start(stopped, 2, "sibling", 2.0);
    AnimationId stoppable_id = 0;
    std::size_t stopped_count = 0;
    stoppable_id = start(stopped, 2, "finish", 1.0, FillMode::Forwards, [&](AnimationId id) {
        if (!expect(id == stoppable_id, "stop callback id")) {
            return;
        }
        stopped_count = stopped.stop(sibling);
    });
    const auto stop_result = stopped.tick(1.0);
    if (!expect(finished_has(stop_result, stoppable_id), "stop finished") ||
        !expect(stopped_count == 1, "sibling stopped") ||
        !expect(stop_result.samples.size() == 1, "stopped sample dropped") ||
        !expect(sample_for(stop_result, sibling) == nullptr, "no sibling sample") ||
        !expect(sample_for(stop_result, stoppable_id) != nullptr, "finisher fill sample") ||
        !expect(stopped.settled() == 1, "finisher retained")) {
        return 2;
    }

    AnimationRuntime restarted;
    AnimationId restart_id = 0;
    bool restarted_ok = false;
    restart_id = start(restarted, 3, "fade", 1.0, FillMode::Forwards, [&](AnimationId id) {
        if (!expect(id == restart_id, "restart callback id")) {
            return;
        }
        restarted_ok = restarted.restart(id);
    });
    const auto restart_result = restarted.tick(1.0);
    const std::string* restart_value = opacity_for(sample_for(restart_result, restart_id));
    if (!expect(restarted_ok, "fill restarted") ||
        !expect(restart_result.samples.size() == 1, "one sample") ||
        !expect(restart_value != nullptr && near(std::stof(*restart_value), 0.0f),
                "restart sample at zero") ||
        !expect(restarted.active() == 1 && restarted.settled() == 0, "restart state") ||
        !expect(restarted.entries().front().elapsed == 0.0, "restart elapsed")) {
        return 3;
    }

    AnimationRuntime replaced;
    AnimationId first_id = 0;
    AnimationId replacement = 0;
    first_id = start(replaced, 4, "fade", 1.0, FillMode::Forwards, [&](AnimationId id) {
        if (!expect(id == first_id, "start callback id")) {
            return;
        }
        replacement = start(replaced, 4, "fade", 2.0);
    });
    const auto replace_result = replaced.tick(1.0);
    const std::string* replacement_value = opacity_for(sample_for(replace_result, replacement));
    if (!expect(replacement != 0 && replacement != first_id, "replacement created") ||
        !expect(replace_result.samples.size() == 1, "old fill sample dropped") ||
        !expect(sample_for(replace_result, first_id) == nullptr, "no old sample") ||
        !expect(replacement_value != nullptr && near(std::stof(*replacement_value), 0.0f),
                "replacement sample at zero") ||
        !expect(replaced.active() == 1 && replaced.settled() == 0, "replacement state")) {
        return 4;
    }

    std::cout << "animation_tick_reentrancy: ok\n";
    return 0;
}
