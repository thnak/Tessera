// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/animation/transition_runtime.hpp"

#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace tessera::ui::animation;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "animation_event: " << message << "\n";
    }
    return condition;
}

KeyframeBlock block(std::string name) {
    KeyframeBlock result;
    result.name = std::move(name);
    result.steps.push_back(KeyframeStep{0.0, {Declaration{"transform", "0", false, 0}}});
    result.steps.push_back(KeyframeStep{100.0, {Declaration{"transform", "1", false, 0}}});
    return result;
}

AnimationId start(AnimationRuntime& runtime, std::uint64_t target, std::string name,
                  double duration = 1.0, std::string property = {}) {
    AnimationInput input;
    input.target = target;
    input.keyframes = block(std::move(name));
    input.timing.duration = duration;
    input.property = std::move(property);
    return runtime.start(std::move(input));
}

} // namespace

int main() {
    {
        AnimationRuntime runtime;
        std::vector<AnimationEvent> seen;
        runtime.add_event_listener([&](const AnimationEvent& e) { seen.push_back(e); });
        const AnimationId id = start(runtime, 5, "spin", 1.0);
        (void)runtime.tick(0.5);
        if (!expect(runtime.pending_events() == 0 && runtime.dispatch_events() == 0, "early") ||
            !expect(seen.empty(), "no event before finish")) {
            return 1;
        }
        (void)runtime.tick(0.5);
        if (!expect(seen.empty(), "tick alone never invokes listeners") ||
            !expect(runtime.pending_events() == 1, "event queued")) {
            return 2;
        }
        if (!expect(runtime.dispatch_events() == 1, "dispatched") ||
            !expect(seen.size() == 1 && seen[0].id == id && seen[0].target == 5 &&
                        seen[0].name == "spin" && seen[0].kind == AnimationKind::Animation &&
                        seen[0].elapsed == 1.0,
                    "event payload") ||
            !expect(runtime.dispatch_events() == 0 && seen.size() == 1, "fires once")) {
            return 3;
        }
    }

    {
        AnimationRuntime runtime;
        int count = 0;
        runtime.add_event_listener([&](const AnimationEvent&) { ++count; });
        (void)start(runtime, 1, "a", 1.0);
        (void)start(runtime, 1, "b", 1.0, "opacity");
        (void)runtime.tick(0.5);
        (void)runtime.stop_target_name(1, "a");
        (void)runtime.stop_target_property(1, "opacity");
        (void)start(runtime, 2, "d", 1.0);
        runtime.clear();
        (void)runtime.tick(2.0);
        runtime.dispatch_events();
        if (!expect(count == 0, "stopped/cleared animations never fire")) {
            return 4;
        }
    }

    {
        AnimationRuntime runtime;
        std::vector<AnimationEvent> seen;
        runtime.add_event_listener([&](const AnimationEvent& e) { seen.push_back(e); });
        TransitionSpec spec;
        spec.property = "opacity";
        spec.duration = 1.0f;
        (void)start_transition(runtime, 3, spec, "opacity", 0.0, 1.0);
        (void)runtime.tick(0.5);
        // Superseded mid-flight: only the replacement reports.
        const AnimationId second = start_transition(runtime, 3, spec, "opacity", 0.5, 0.0);
        (void)runtime.tick(1.0);
        runtime.dispatch_events();
        if (!expect(seen.size() == 1, "superseded transition silent") ||
            !expect(seen[0].id == second && seen[0].kind == AnimationKind::Transition &&
                        seen[0].name == "opacity" && seen[0].property == "opacity" &&
                        seen[0].target == 3,
                    "transition payload")) {
            return 5;
        }
    }

    {
        AnimationRuntime runtime;
        runtime.set_reduced_motion(true);
        std::vector<AnimationEvent> seen;
        runtime.add_event_listener([&](const AnimationEvent& e) { seen.push_back(e); });
        (void)start(runtime, 9, "slide", 2.0);
        (void)runtime.tick(0.0);
        runtime.dispatch_events();
        if (!expect(seen.size() == 1 && seen[0].elapsed == 0.0,
                    "reduced motion clamps and fires")) {
            return 6;
        }
    }

    {
        AnimationRuntime runtime;
        std::vector<std::string> order;
        AnimationListenerId second_id = 0;
        const AnimationListenerId first_id =
            runtime.add_event_listener([&](const AnimationEvent& e) {
                order.push_back("first:" + e.name);
                if (e.name == "one") {
                    (void)start(runtime, 1, "two", 1.0);
                    (void)runtime.remove_event_listener(second_id);
                }
            });
        second_id = runtime.add_event_listener(
            [&](const AnimationEvent& e) { order.push_back("second:" + e.name); });
        (void)start(runtime, 1, "one", 1.0);
        (void)runtime.tick(1.0);
        runtime.dispatch_events();
        if (!expect(order.size() == 1 && order[0] == "first:one", "removed listener skipped") ||
            !expect(runtime.active() == 1, "listener started animation")) {
            return 7;
        }
        (void)runtime.tick(1.0);
        runtime.dispatch_events();
        if (!expect(order.size() == 2 && order[1] == "first:two", "new animation reports") ||
            !expect(runtime.remove_event_listener(first_id) &&
                        !runtime.remove_event_listener(first_id),
                    "remove listener")) {
            return 8;
        }
    }

    std::cout << "animation_event: ok\n";
    return 0;
}
