// SPDX-License-Identifier: MIT
//
// The host half of issue #187's acceptance: "the host updates reducedMotion and the media queries".
//
// Two things are proven here, both deterministically and without a desktop:
//
//   1. `AppHost` with the re-resolvable prepared sheet resolves `@media (prefers-color-scheme)` and
//      `@media (prefers-reduced-motion)` against the OS appearance it was built with, and a later
//      `set_appearance` re-resolves the SAME prepared data so the stylesheet's matching rules move
//      with the preference (ADR-048's resolve step, not a re-parse).
//   2. The fake window's normalized event seam: an appearance change becomes
//      `ThemeChanged`/`SystemSettingsChanged`, and the snapshot the app re-reads is the changed
//      one. That is the same seam `tests/unit/harness/os_event_display_tests.cpp` drives for
//      display capabilities, so the event path is covered without a bus or a window.
//
// `reducedMotion` itself is asserted through the host's animation runtime, the spring runtime and
// the media context, which are the three consumers the acceptance names.

#include "fake_hal.hpp"
#include "tessera/app/host.hpp"
#include "tessera/hal/appearance.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/media_query.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace tessera;
using tessera::app::AppHost;
using tessera::app::HostConfig;
using tessera::hal::AppearancePreference;
using tessera::hal::ColorSchemePreference;
using tessera::ui::Node;
using tessera::ui::style::ColorScheme;
using tessera::ui::style::MediaContext;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_host_appearance: " << message << "\n";
        ++failures;
    }
    return condition;
}

// The stylesheet under test: one rule per media feature, each with a distinct background color so
// the cascade's decision is readable from the computed style rather than from a pixel.
constexpr std::string_view kMediaCss = R"(
.root { background: #000000; }
@media (prefers-color-scheme: dark) { .root { background: #111111; } }
@media (prefers-color-scheme: light) { .root { background: #222222; } }
@media (prefers-reduced-motion: reduce) { .root { background: #333333; } }
)";

AppearancePreference measured(ColorSchemePreference scheme, bool reduced_motion) {
    AppearancePreference appearance;
    appearance.color_scheme = scheme;
    appearance.reduced_motion = reduced_motion;
    appearance.assumed.color_scheme = 0;
    appearance.assumed.reduced_motion = 0;
    return appearance;
}

// The background the host's stylesheet resolves for the root right now. The style provider is the
// same route paint takes, so this asserts the CASCADE result of the media queries, not just the
// `MediaContext` fields the queries are evaluated against.
std::string resolved_background(AppHost& host, const Node& node) {
    const tessera::ui::style::ComputedStyle style = host.styles().compute(node);
    const std::string* const background = style.find("background-color");
    return background == nullptr ? std::string{} : *background;
}

} // namespace
int main() {
    // --- 1. The stylesheet resolves against the startup appearance ---------------------------
    {
        tessera::runtime::ManualClock clock;
        tessera::runtime::RuntimeLoop loop(clock);
        Node root(1);
        root.set_class_name("root");
        const tessera::ui::style::PreparedStyleSheet prepared =
            tessera::ui::style::prepare_stylesheet(kMediaCss);

        HostConfig config;
        config.width = 200.0f;
        config.height = 120.0f;
        config.appearance = measured(ColorSchemePreference::Dark, false);
        AppHost host(root, prepared, loop, config);
        (void)host.begin_frame(0.0);

        expect(host.media_context().color_scheme == ColorScheme::Dark,
               "a dark OS preference resolves prefers-color-scheme to dark");
        expect(!host.media_context().prefers_reduced_motion,
               "motion enabled resolves prefers-reduced-motion to no-preference");
        expect(!host.reduced_motion(), "reducedMotion starts false when the OS says motion is on");

        // The media query itself, evaluated through the engine, not just the context field.
        const auto dark_query =
            tessera::ui::style::parse_media_query_list("(prefers-color-scheme: dark)");
        expect(tessera::ui::style::evaluate(dark_query.list, host.media_context()),
               "the dark media query matches a dark appearance");
        const auto light_query =
            tessera::ui::style::parse_media_query_list("(prefers-color-scheme: light)");
        expect(!tessera::ui::style::evaluate(light_query.list, host.media_context()),
               "the light media query does not match a dark appearance");
        // The cascade result, not only the context: compile the same CSS against the host's media
        // context and check the dark rule won. This is the resolve step `set_appearance` drives, so
        // it proves the stylesheet (not just the boolean) follows the OS preference.
        const tessera::ui::style::CompiledStyleSheet resolved(
            prepared, tessera::ui::style::Origin::Author, {}, host.media_context());
        expect(resolved.media().color_scheme == ColorScheme::Dark,
               "the host's media context is what the resolved sheet reports");
        expect(!resolved.media().prefers_reduced_motion, "motion is not reduced at startup");
        // And the CASCADE result itself: the dark rule's declaration is what paint resolves.
        expect(resolved_background(host, root) == "#111111",
               "the dark @media rule is the declaration the cascade resolves");
    }

    // --- 2. A runtime appearance change updates reducedMotion AND the media queries -----------
    {
        tessera::runtime::ManualClock clock;
        tessera::runtime::RuntimeLoop loop(clock);
        Node root(1);
        root.set_class_name("root");
        const tessera::ui::style::PreparedStyleSheet prepared =
            tessera::ui::style::prepare_stylesheet(kMediaCss);

        HostConfig config;
        config.width = 200.0f;
        config.height = 120.0f;
        config.appearance = measured(ColorSchemePreference::Light, false);
        AppHost host(root, prepared, loop, config);
        (void)host.begin_frame(0.0);

        const auto light_query =
            tessera::ui::style::parse_media_query_list("(prefers-color-scheme: light)");
        const auto reduce_query =
            tessera::ui::style::parse_media_query_list("(prefers-reduced-motion: reduce)");
        expect(tessera::ui::style::evaluate(light_query.list, host.media_context()),
               "the light media query matches the startup appearance");

        // The OS switched to dark and asked for reduced motion.
        host.set_appearance(measured(ColorSchemePreference::Dark, true));
        expect(host.reduced_motion(), "reducedMotion follows the OS preference");
        expect(host.animations().reduced_motion(),
               "the animation runtime receives the reduced-motion flag");
        // The frame pipeline mirrors the animation runtime's flag onto the springs at the tick
        // boundary (it is frame state, not host state), so one frame is what makes it observable.
        (void)host.begin_frame(0.0);
        expect(host.springs().reduced_motion(),
               "the spring runtime receives the reduced-motion flag after the frame boundary");
        expect(host.media_context().color_scheme == ColorScheme::Dark,
               "the media context's color scheme follows the OS preference");
        expect(host.media_context().prefers_reduced_motion,
               "the media context's reduced-motion flag follows the OS preference");
        const auto dark_query =
            tessera::ui::style::parse_media_query_list("(prefers-color-scheme: dark)");
        expect(tessera::ui::style::evaluate(dark_query.list, host.media_context()),
               "prefers-color-scheme: dark now matches");
        expect(!tessera::ui::style::evaluate(light_query.list, host.media_context()),
               "prefers-color-scheme: light no longer matches");
        expect(tessera::ui::style::evaluate(reduce_query.list, host.media_context()),
               "prefers-reduced-motion: reduce now matches");

        // Back to light/motion-on: both move back, proving the change is not latched one way.
        host.set_appearance(measured(ColorSchemePreference::Light, false));
        expect(!host.reduced_motion(), "reducedMotion clears when the OS says motion is on");
        expect(host.media_context().color_scheme == ColorScheme::Light,
               "the media context returns to light");
        expect(tessera::ui::style::evaluate(light_query.list, host.media_context()),
               "the light media query matches again");
    }

    // --- 3. A no-op appearance report changes nothing ---------------------------------------
    {
        tessera::runtime::ManualClock clock;
        tessera::runtime::RuntimeLoop loop(clock);
        Node root(1);
        root.set_class_name("root");
        const tessera::ui::style::PreparedStyleSheet prepared =
            tessera::ui::style::prepare_stylesheet(kMediaCss);
        HostConfig config;
        config.appearance = measured(ColorSchemePreference::Dark, true);
        AppHost host(root, prepared, loop, config);
        (void)host.begin_frame(0.0);
        const bool before = host.reduced_motion();
        host.set_appearance(measured(ColorSchemePreference::Dark, true));
        expect(host.reduced_motion() == before && host.reduced_motion(),
               "a settings broadcast that changed nothing leaves the host as it was");
    }

    // --- 4. The fake window's normalized event seam (the platform half) ----------------------
    {
        harness::testing::FakeWindow window;
        // A platform with no settings source answers the documented fallback: light, motion on,
        // both recorded as assumptions. The app then behaves exactly as it did before #187.
        const AppearancePreference fallback = window.appearance();
        expect(fallback.color_scheme == ColorSchemePreference::Unspecified &&
                   !fallback.reduced_motion && fallback.assumed.color_scheme == 1,
               "a fake window with no scripted appearance reports the documented fallback");

        // A dark-mode change arrives as the normalized theme event and the snapshot moved.
        window.set_appearance(measured(ColorSchemePreference::Dark, false));
        expect(window.appearance().color_scheme == ColorSchemePreference::Dark,
               "the changed snapshot is what the app re-reads");
        expect(window.has_queued(hal::WindowEventType::ThemeChanged),
               "a color-scheme change arrives as the normalized ThemeChanged event");

        // A reduced-motion toggle without a color-scheme change is the settings event the Windows
        // path already used, so both platforms share one taxonomy.
        window.set_appearance(measured(ColorSchemePreference::Dark, true));
        expect(window.has_queued(hal::WindowEventType::SystemSettingsChanged),
               "a motion change arrives as the normalized settings event");

        // A broadcast that did not move the value produces no event at all.
        std::array<hal::WindowEvent, 8> events{};
        const std::size_t drained = window.poll_events(events);
        const std::size_t before = window.queued_events();
        window.set_appearance(measured(ColorSchemePreference::Dark, true));
        expect(window.queued_events() == before, "a no-op settings broadcast produces no event");
        expect(drained > 0, "the queued change events were drainable through poll_events");
    }

    if (failures == 0) {
        std::cout << "app_host_appearance: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
