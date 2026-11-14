// SPDX-License-Identifier: MIT
//
// WindowHost's accessibility controller, Windows UIA half (issue #185,
// docs/specs/accessibility-v1.md section 12.3). Every test runs against the fake apartment + fake
// UIA platform seams from the shared a11y fixture: no screen reader, desktop session or COM client.
// The Linux AT-SPI half is in app_accessibility_host_atspi_tests.cpp; both link into this
// executable.

#include "app_accessibility_host_fixture.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace app_a11y_host_test {
namespace {

// (a) The root provider is installed on the window after a successful start, uninstalled on stop,
// and the fake's opaque host token reached the platform seam.
bool root_install_and_uninstall() {
    auto harness = make_host();
    Log log;
    FakeWindow window;
    AccessibilityHost controller;
    const bool started = controller.start(
        window, [](A11ySnapshot& out) { build_button_tree(out); }, {},
        uia_config(harness.get(), log));
    if (!expect(started, "the UIA controller starts") ||
        !expect(controller.enabled(), "the controller reports enabled") ||
        !expect(window.accessibility_root_installed(), "the root provider is installed") ||
        !expect(controller.uia_host_for_testing() != nullptr, "the controller owns the UIA host")) {
        return false;
    }
    // The fake's non-null token reached the platform seam (the window's HWND equivalent).
    if (!expect(harness->platform->handle_.valid(), "the host handle reached the platform seam") ||
        !expect(harness->log.count("platform.set_handle") == 1,
                "the handle was installed exactly once")) {
        return false;
    }
    // The root answer is the real WM_GETOBJECT path: not ready yet, then ready after a bridge step.
    void* provider = nullptr;
    const tessera::hal::AccessibilityRoot installed = window.accessibility_root();
    if (!expect(installed.answer != nullptr && installed.context != nullptr,
                "the installed answer carries the controller's host")) {
        return false;
    }
    // The bridge publishes the root on its worker thread; a barrier makes that deterministic.
    if (!expect(controller.uia_host_for_testing()->bridge().step().has_value(),
                "the start hook runs")) {
        return false;
    }
    if (!expect(installed.answer(installed.context, tessera::ui::a11y::kUiaRootObjectId, provider),
                "the root answers WM_GETOBJECT once published") ||
        !expect(provider != nullptr, "the answer returns the raw provider")) {
        return false;
    }
    controller.stop();
    return expect(!window.accessibility_root_installed(), "stop uninstalls the root") &&
           expect(!controller.enabled(), "stop disables the controller") &&
           expect(controller.uia_host_for_testing() == nullptr, "stop releases the UIA host");
}

// (b) A failed/unsupported start turns accessibility off, reports exactly one diagnostic and never
// throws. The window is untouched (no root left installed).
bool unsupported_start_is_one_diagnostic() {
    Log log;
    FakeWindow window;
    AccessibilityHost controller;
    bool threw = false;
    bool started = false;
    try {
        started = controller.start(
            window, [](A11ySnapshot& out) { build_button_tree(out); }, {},
            unsupported_uia_config(log));
    } catch (...) {
        threw = true;
    }
    if (!expect(!threw, "a failed start does not throw") ||
        !expect(!started, "the unsupported start returns false") ||
        !expect(!controller.enabled(), "accessibility is off") ||
        !expect(!window.accessibility_root_installed(), "no root is left installed")) {
        return false;
    }
    const AccessibilityHostStatus& status = controller.status();
    if (!expect(status.failed, "the status reports the failure") ||
        !expect(status.error.find("unsupported-platform") != std::string::npos,
                "the status carries the error name") ||
        !expect(log.lines.size() == 1, "exactly one diagnostic was reported")) {
        return false;
    }
    const std::string& line = log.lines[0];
    if (!expect(line.rfind("TESSERA_A11Y_UNSUPPORTED:", 0) == 0,
                "the diagnostic uses the documented prefix") ||
        !expect(line.find("uia") != std::string::npos, "the diagnostic names the backend") ||
        !expect(line.find("unsupported-platform") != std::string::npos,
                "the diagnostic carries the error name")) {
        return false;
    }
    // Repeated ticks after the failure stay silent: the diagnostic is latched and no build runs.
    for (int index = 0; index < 3; ++index) {
        (void)controller.tick();
    }
    return expect(log.lines.size() == 1, "later ticks report nothing new") &&
           expect_eq(static_cast<std::int64_t>(controller.status().snapshots), 0,
                     "a disabled controller builds nothing");
}

// (c) A static window does no snapshot work: repeated ticks without a dirty mark call the source
// zero times and publish nothing.
bool static_window_builds_nothing() {
    auto harness = make_host();
    Log log;
    FakeWindow window;
    std::size_t builds = 0;
    AccessibilityHost controller;
    if (!expect(controller.start(
                    window,
                    [&builds](A11ySnapshot& out) {
                        ++builds;
                        build_button_tree(out);
                    },
                    {}, uia_config(harness.get(), log)),
                "the controller starts")) {
        return false;
    }
    // The successful start marks dirty once (the first tree must be published), so the first tick
    // is the one build. Everything after it must be free.
    (void)controller.tick();
    const std::int64_t after_first = static_cast<std::int64_t>(builds);
    for (int index = 0; index < 8; ++index) {
        (void)controller.tick();
    }
    if (!expect_eq(static_cast<std::int64_t>(builds), after_first, "static ticks build nothing") ||
        !expect(!controller.dirty(), "the controller stays clean")) {
        return false;
    }
    // A static tick also publishes nothing to the bridge: the handoff still holds the one tree.
    const auto& handoff = controller.uia_host_for_testing()->bridge().handoff_stats();
    return expect_eq(static_cast<std::int64_t>(handoff.published), 1,
                     "only the first tree was published");
}

// (d) A size change or an explicit dirty mark rebuilds exactly once.
bool dirty_and_resize_rebuild_once() {
    auto harness = make_host();
    Log log;
    FakeWindow window;
    std::size_t builds = 0;
    AccessibilityHost controller;
    if (!expect(controller.start(
                    window,
                    [&builds](A11ySnapshot& out) {
                        ++builds;
                        build_button_tree(out);
                    },
                    {}, uia_config(harness.get(), log)),
                "the controller starts")) {
        return false;
    }
    (void)controller.tick();
    const std::int64_t base = static_cast<std::int64_t>(builds);

    // An explicit dirty mark: exactly one rebuild, the following tick is clean.
    controller.mark_dirty();
    (void)controller.tick();
    if (!expect_eq(static_cast<std::int64_t>(builds), base + 1, "one dirty tick builds once")) {
        return false;
    }
    (void)controller.tick();
    if (!expect_eq(static_cast<std::int64_t>(builds), base + 1, "the next tick is clean")) {
        return false;
    }

    // A client-area change is its own dirty source: the window's metrics moved, so one tick
    // rebuilds even though nothing called mark_dirty().
    window.resize(640, 480);
    (void)controller.tick();
    if (!expect_eq(static_cast<std::int64_t>(builds), base + 2, "a resize rebuilds once")) {
        return false;
    }
    (void)controller.tick();
    return expect_eq(static_cast<std::int64_t>(builds), base + 2, "the size settles");
}

// (e) Publishing reaches the bridge and the published tree is the one the source built.
bool published_tree_reaches_the_bridge() {
    auto harness = make_host();
    Log log;
    FakeWindow window;
    AccessibilityHost controller;
    if (!expect(controller.start(
                    window, [](A11ySnapshot& out) { build_button_tree(out); }, {},
                    uia_config(harness.get(), log)),
                "the controller starts")) {
        return false;
    }
    (void)controller.tick();
    if (!expect(controller.uia_host_for_testing()->bridge().step().has_value(),
                "the bridge adopts the tree")) {
        return false;
    }
    std::size_t nodes = 0;
    std::string root_name;
    const bool served =
        controller.uia_host_for_testing()
            ->bridge()
            .run_on_bridge([&](tessera::ui::a11y::UiaProvider& provider) {
                nodes = provider.node_count();
                if (const A11yNode* root = provider.node(provider.root()); root != nullptr) {
                    root_name.assign(tessera::ui::a11y::name_of(*root));
                }
            })
            .has_value();
    const AccessibilityHostStatus& status = controller.status();
    return expect(served, "the provider answers") &&
           expect_eq(static_cast<std::int64_t>(nodes), 2, "both nodes reached the bridge") &&
           expect(root_name == "Root", "the root name reached the bridge") &&
           expect_eq(static_cast<std::int64_t>(status.publishes), 1, "one publish was counted") &&
           expect_eq(static_cast<std::int64_t>(status.snapshots), 1, "one build was counted") &&
           expect_eq(static_cast<std::int64_t>(status.nodes), 2, "the node count is reported");
}

// A drained action reaches the scene handler with the snapshot's own NodeId (the index -> id
// mapping is what makes an accessibility action safe against a replaced tree).
bool drained_action_maps_to_node_id() {
    auto harness = make_host();
    Log log;
    FakeWindow window;
    std::vector<AccessibilityAction> drained;
    AccessibilityHost controller;
    if (!expect(controller.start(
                    window, [](A11ySnapshot& out) { build_button_tree(out, 4242); },
                    [&drained](const AccessibilityAction& action) { drained.push_back(action); },
                    uia_config(harness.get(), log)),
                "the controller starts")) {
        return false;
    }
    (void)controller.tick();
    if (!expect(controller.uia_host_for_testing()->bridge().step().has_value(),
                "the bridge adopts the tree")) {
        return false;
    }
    bool queued = false;
    const bool ran = controller.uia_host_for_testing()
                         ->bridge()
                         .run_on_bridge([&](tessera::ui::a11y::UiaProvider& provider) {
                             queued = provider.request_action(1, Action::Press).has_value();
                         })
                         .has_value();
    if (!expect(ran && queued, "the client request was queued")) {
        return false;
    }
    // The drain happens on the next tick even with no dirty tree (an action is not a snapshot
    // change), which is the documented tick-boundary contract.
    (void)controller.tick();
    return expect_eq(static_cast<std::int64_t>(drained.size()), 1,
                     "one action reached the scene") &&
           expect_eq(static_cast<std::int64_t>(drained[0].node), 4242,
                     "the action carries the snapshot's NodeId, not the index") &&
           expect(drained[0].action == Action::Press, "the action kind is preserved") &&
           expect_eq(static_cast<std::int64_t>(controller.status().actions), 1,
                     "the controller counted the action");
}

} // namespace

// The AT-SPI cases are compiled into this executable from the sibling translation unit.
} // namespace app_a11y_host_test

int main() {
    using namespace app_a11y_host_test;
    if (!root_install_and_uninstall()) {
        return 1;
    }
    if (!unsupported_start_is_one_diagnostic()) {
        return 2;
    }
    if (!static_window_builds_nothing()) {
        return 3;
    }
    if (!dirty_and_resize_rebuild_once()) {
        return 4;
    }
    if (!published_tree_reaches_the_bridge()) {
        return 5;
    }
    if (!drained_action_maps_to_node_id()) {
        return 6;
    }
    if (!atspi_handshake_starts_on_the_worker()) {
        return 7;
    }
    if (!atspi_auth_rejected_turns_accessibility_off()) {
        return 8;
    }
    if (!atspi_closed_peer_turns_accessibility_off()) {
        return 9;
    }
    if (failures == 0) {
        std::cout << "app_accessibility_host: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
