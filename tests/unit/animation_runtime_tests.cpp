// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/runtime.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::ui::Dirty;
using tessera::ui::animation::AnimationId;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_runtime: " << message << "\n";
    }
    return condition;
}

bool near(double lhs, double rhs) {
    return std::abs(lhs - rhs) <= 0.001;
}

Declaration declaration(std::string property, std::string value) {
    return Declaration{std::move(property), std::move(value), false, 0};
}

KeyframeStep step(double offset, std::string property, std::string value) {
    return KeyframeStep{offset, {declaration(std::move(property), std::move(value))}};
}

KeyframeBlock opacity_block() {
    KeyframeBlock block;
    block.name = "pulse";
    block.steps.push_back(step(0.0, "opacity", "0"));
    block.steps.push_back(step(50.0, "opacity", "1"));
    block.steps.push_back(step(100.0, "opacity", "0"));
    return block;
}

KeyframeBlock width_block() {
    KeyframeBlock block;
    block.name = "grow";
    block.steps.push_back(step(0.0, "width", "10px"));
    block.steps.push_back(step(100.0, "width", "20px"));
    return block;
}

const std::string* value(const std::vector<Declaration>& declarations, std::string_view property) {
    for (const Declaration& declaration : declarations) {
        if (declaration.property == property) {
            return &declaration.value;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    AnimationRuntime runtime;
    int completions = 0;
    AnimationId completed = 0;

    KeyframeTiming timing;
    timing.duration = 1.0;
    const AnimationId pulse =
        runtime.start(AnimationInput{42, opacity_block(), timing, [&](AnimationId id) {
                                         ++completions;
                                         completed = id;
                                     }});
    if (!expect(runtime.active() == 1, "start adds entry") || !expect(pulse != 0, "id assigned")) {
        return 1;
    }

    const auto quarter = runtime.tick(0.25);
    if (!expect(quarter.samples.size() == 1, "quarter sample") ||
        !expect(quarter.samples[0].target == 42, "target copied") ||
        !expect(value(quarter.samples[0].declarations, "opacity") != nullptr, "opacity emitted") ||
        !expect(near(std::stod(*value(quarter.samples[0].declarations, "opacity")), 0.5),
                "quarter value") ||
        !expect(tessera::ui::has_flag(quarter.dirty, Dirty::Paint), "paint dirty") ||
        !expect(tessera::ui::has_flag(quarter.dirty, Dirty::Render), "render dirty") ||
        !expect(!tessera::ui::has_flag(quarter.dirty, Dirty::Layout), "opacity not layout") ||
        !expect(quarter.keep_awake, "active animation keeps awake") ||
        !expect(quarter.finished.empty(), "not finished")) {
        return 2;
    }

    const auto end = runtime.tick(0.75);
    if (!expect(end.finished.size() == 1 && end.finished[0] == pulse, "finished reported") ||
        !expect(end.samples.empty(), "fill none emits no end sample") ||
        !expect(!end.keep_awake, "finished animation releases wake") ||
        !expect(runtime.empty(), "entry removed") ||
        !expect(completions == 1 && completed == pulse, "completion callback")) {
        return 3;
    }

    KeyframeTiming forwards = timing;
    forwards.fill = FillMode::Forwards;
    const AnimationId forward_id = runtime.start(AnimationInput{7, opacity_block(), forwards, {}});
    if (!expect(forward_id != 0, "forward id assigned")) {
        return 4;
    }
    const auto forwarded = runtime.tick(1.0);
    if (!expect(forwarded.finished.size() == 1, "forward finished") ||
        !expect(forwarded.samples.size() == 1, "forward final sample") ||
        !expect(near(std::stod(*value(forwarded.samples[0].declarations, "opacity")), 0.0),
                "forward final value") ||
        !expect(runtime.empty(), "forward removed")) {
        return 5;
    }

    const AnimationId width_id = runtime.start(AnimationInput{9, width_block(), timing, {}});
    if (!expect(width_id != 0, "width id assigned")) {
        return 6;
    }
    const auto layout = runtime.tick(0.5);
    if (!expect(tessera::ui::has_flag(layout.dirty, Dirty::Layout), "width marks layout") ||
        !expect(tessera::ui::has_flag(layout.dirty, Dirty::Paint), "width marks paint") ||
        !expect(value(layout.samples[0].declarations, "width") != nullptr, "width emitted") ||
        !expect(*value(layout.samples[0].declarations, "width") == "15px", "width interpolated")) {
        return 7;
    }
    runtime.clear();
    if (!expect(runtime.empty(), "clear removes entries")) {
        return 8;
    }

    const AnimationId stopped = runtime.start(AnimationInput{10, opacity_block(), timing, {}});
    if (!expect(runtime.stop(stopped), "stop existing") ||
        !expect(!runtime.stop(stopped), "stop missing") ||
        !expect(runtime.empty(), "stopped removed")) {
        return 9;
    }

    AnimationRuntime restart;
    AnimationId restarted = 0;
    const AnimationId before = restart.start(AnimationInput{
        11, opacity_block(), timing, [&](AnimationId) {
            restarted = restart.start(AnimationInput{12, opacity_block(), timing, {}});
        }});
    if (!expect(before != 0, "callback source started")) {
        return 10;
    }
    const auto callback_tick = restart.tick(1.0);
    if (!expect(callback_tick.finished.size() == 1, "callback source finished") ||
        !expect(restarted != 0, "callback starts next") ||
        !expect(restart.active() == 1, "callback animation active") ||
        !expect(callback_tick.keep_awake, "callback keeps awake")) {
        return 11;
    }

    AnimationRuntime infinite_runtime;
    KeyframeTiming infinite = timing;
    infinite.iterations = std::numeric_limits<double>::infinity();
    const AnimationId forever =
        infinite_runtime.start(AnimationInput{13, opacity_block(), infinite, {}});
    const auto long_tick = infinite_runtime.tick(100.0);
    if (!expect(forever != 0, "infinite id") ||
        !expect(long_tick.finished.empty(), "infinite not finished") ||
        !expect(long_tick.samples.size() == 1, "infinite sampled") ||
        !expect(infinite_runtime.active() == 1, "infinite active") ||
        !expect(long_tick.keep_awake, "infinite keeps awake")) {
        return 12;
    }

    const double elapsed_before = infinite_runtime.entries()[0].elapsed;
    const auto bad_tick = infinite_runtime.tick(std::nan(""));
    if (!expect(bad_tick.samples.empty(), "nan dt ignored") ||
        !expect(near(infinite_runtime.entries()[0].elapsed, elapsed_before), "nan dt no advance")) {
        return 13;
    }

    std::cout << "animation_runtime: ok\n";
    return 0;
}
