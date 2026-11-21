// SPDX-License-Identifier: MIT
//
// `useBreakpoint()` end to end (docs/specs/jsx-subset-v1.md section 7.1, issue #692): the host
// computes the class from the same `MediaContext` the `@media` evaluator reads, pushes it to the
// registered consumers after a size change, and a resize inside one class pushes nothing. Headless:
// ManualClock, no GPU.

#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/responsive/environment_set.hpp"
#include "tessera/ui/responsive/size_class.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::Node;
using tessera::ui::responsive::EnvironmentConsumer;
using tessera::ui::responsive::EnvironmentSet;
using tessera::ui::responsive::size_class_name;
using tessera::ui::responsive::SizeClass;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "breakpoint_host: " << message << "\n";
    }
    return condition;
}

// Records every value the host pushes, so a test asserts the sequence rather than the final state.
struct Recorder {
    static void set(void* context, std::string_view value) {
        static_cast<Recorder*>(context)->values.emplace_back(value);
    }
    std::vector<std::string> values;
};

int host_pushes_the_class() {
    Node root(1);
    auto sheet = tessera::ui::style::compile_stylesheet("* { width: 100%; }");
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = 320.0f;
    config.height = 240.0f;
    tessera::app::AppHost host(root, std::move(sheet), loop, config);

    Recorder recorder;
    host.environment_values().attach(EnvironmentConsumer{"breakpoint", &Recorder::set, &recorder});

    // The host starts at the class its window has, so a component that reads the hook renders the
    // right layout on the first frame rather than the field's initial value.
    if (!expect(host.size_class() == SizeClass::Compact, "320 px is compact")) {
        return 1;
    }
    (void)host.begin_frame(0.0);
    host.end_frame();

    // Crossing 600 pushes `medium` exactly once.
    host.resize(700.0f, 240.0f, 1.0f);
    (void)host.begin_frame(1.0 / 60.0);
    host.end_frame();
    if (!expect(host.size_class() == SizeClass::Medium, "700 px is medium") ||
        !expect(recorder.values.size() == 1 && recorder.values.back() == "medium",
                "crossing 600 pushed medium once")) {
        return 2;
    }

    // A resize inside the same class pushes nothing: the set is not even walked.
    host.resize(800.0f, 240.0f, 1.0f);
    (void)host.begin_frame(1.0 / 60.0);
    host.end_frame();
    if (!expect(recorder.values.size() == 1, "a resize inside one class pushes nothing")) {
        return 3;
    }

    // Crossing 840 pushes `expanded`.
    host.resize(900.0f, 240.0f, 1.0f);
    (void)host.begin_frame(1.0 / 60.0);
    host.end_frame();
    if (!expect(recorder.values.size() == 2 && recorder.values.back() == "expanded",
                "crossing 840 pushed expanded")) {
        return 4;
    }

    // And back down.
    host.resize(500.0f, 240.0f, 1.0f);
    (void)host.begin_frame(1.0 / 60.0);
    host.end_frame();
    if (!expect(recorder.values.size() == 3 && recorder.values.back() == "compact",
                "crossing back down pushed compact")) {
        return 5;
    }
    return 0;
}

int the_hook_and_the_stylesheet_agree() {
    // The class comes from the media context, which is what `@media` resolves against, so a
    // stylesheet breakpoint at the same width and the hook cannot disagree.
    Node root(1);
    auto sheet = tessera::ui::style::compile_stylesheet(
        "@media (min-width: 600px) { * { background-color: red } }");
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::app::HostConfig config;
    config.width = 599.0f;
    config.height = 240.0f;
    tessera::app::AppHost host(root, std::move(sheet), loop, config);

    if (!expect(host.size_class() == SizeClass::Compact, "599 is compact") ||
        !expect(host.media_context().viewport_width == 599.0, "the context is the window")) {
        return 6;
    }
    host.resize(600.0f, 240.0f, 1.0f);
    if (!expect(host.size_class() == SizeClass::Medium, "600 is medium") ||
        !expect(host.media_context().viewport_width == 600.0, "the context moved with it")) {
        return 7;
    }
    return 0;
}

int the_set_is_provider_scoped() {
    // A push for one provider never reaches a consumer of another, so a theme change cannot
    // overwrite a size class.
    EnvironmentSet set;
    Recorder breakpoints;
    Recorder themes;
    set.attach(EnvironmentConsumer{"breakpoint", &Recorder::set, &breakpoints});
    set.attach(EnvironmentConsumer{"theme", &Recorder::set, &themes});
    if (!expect(set.size() == 2, "two consumers attached")) {
        return 8;
    }
    (void)set.push("breakpoint", "medium");
    if (!expect(breakpoints.values.size() == 1 && themes.values.empty(),
                "a breakpoint push reaches only the breakpoint consumer")) {
        return 9;
    }
    // Attaching the same consumer twice is idempotent: a rebuild that re-attaches is a no-op.
    set.attach(EnvironmentConsumer{"breakpoint", &Recorder::set, &breakpoints});
    if (!expect(set.size() == 2, "re-attaching the same consumer is a no-op")) {
        return 10;
    }
    set.detach(EnvironmentConsumer{"breakpoint", &Recorder::set, &breakpoints});
    if (!expect(set.size() == 1 && set.size_of("breakpoint") == 0,
                "detach removes only that consumer")) {
        return 11;
    }
    return 0;
}

} // namespace

int main() {
    if (const int rc = host_pushes_the_class(); rc != 0) {
        return rc;
    }
    if (const int rc = the_hook_and_the_stylesheet_agree(); rc != 0) {
        return rc;
    }
    if (const int rc = the_set_is_provider_scoped(); rc != 0) {
        return rc;
    }
    std::cout << "breakpoint_host: ok\n";
    return 0;
}
