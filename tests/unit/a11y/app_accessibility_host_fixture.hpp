// SPDX-License-Identifier: MIT
//
// Shared fixture for the `app_accessibility_host` test: the controller's include seam, the
// assertion helpers, the two-node snapshot builder, the diagnostic log, and the two injectable
// configs (a real `UiaHost` over the fake apartment/platform seams, and a failing one). Kept in a
// header so the test translation unit stays under the file-size limit; every test in the file
// drives the same seams.

#pragma once

#include "a11y_session_fixture.hpp"
#include "a11y_uia_host_fixture.hpp"
#include "fake_hal.hpp"
#include "tessera/ui/accessibility/atspi_session.hpp"
#include "tessera/ui/accessibility/uia_host.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// The controller's own header is internal to src/app; the test includes it through the app
// library's private include directory (see tests/unit/a11y/CMakeLists.txt).
#include "accessibility_host.hpp"

namespace app_a11y_host_test {

using namespace a11y_uia_host_test;

using tessera::app::AccessibilityAction;
using tessera::app::AccessibilityBackend;
using tessera::app::AccessibilityHost;
using tessera::app::AccessibilityHostConfig;
using tessera::app::AccessibilityHostStatus;
using tessera::harness::testing::FakeWindow;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::Action;
using tessera::ui::a11y::kMaxTextBytes;
using tessera::ui::a11y::Role;
using tessera::ui::a11y::UiaHost;

inline int failures = 0;

inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_accessibility_host: " << message << "\n";
        ++failures;
    }
    return condition;
}

inline bool expect_eq(std::int64_t actual, std::int64_t expected, const char* message) {
    if (actual == expected) {
        return true;
    }
    std::cerr << "app_accessibility_host: " << message << " (actual=" << actual
              << ", expected=" << expected << ")\n";
    ++failures;
    return false;
}

inline void set_text(std::array<char, kMaxTextBytes>& target, std::string_view text) {
    const std::size_t size = text.size() < kMaxTextBytes ? text.size() : kMaxTextBytes - 1;
    for (std::size_t index = 0; index < size; ++index) {
        target[index] = text[index];
    }
    target[size] = '\0';
}

// A two-node tree: a root pane plus a pressable, focusable button carrying a stable node id. The
// builder is bypassed on purpose: these tests drive the controller's source seam, not the semantics
// builder (a11y_tree has its own suite).
inline void build_button_tree(A11ySnapshot& out, std::uint64_t button_id = 42) {
    out.clear();
    A11yNode root;
    root.id = 1;
    root.role = Role::Generic;
    set_text(root.name, "Root");
    root.child_count = 1;
    root.children[0] = 1;

    A11yNode button;
    button.id = button_id;
    button.role = Role::Button;
    button.states = tessera::ui::a11y::State::Focusable;
    button.actions = Action::Press | Action::Focus;
    set_text(button.name, "Save");
    out.nodes = {root, button};
}

// Records the diagnostic lines the controller reports, so "exactly one" is an assertion rather than
// a stderr scrape.
struct Log {
    std::vector<std::string> lines;

    void add(std::string line) {
        lines.push_back(std::move(line));
    }
};

// A real `UiaHost` over the fixture's fake apartment + fake platform, so the host's lifecycle
// (bridge thread, root publication, tick, stop join) runs unmodified. The harness outlives the
// controller in every test, and the fakes are owned by the host from the moment the factory
// returns.
inline AccessibilityHostConfig uia_config(HostHarness* harness, Log& log) {
    AccessibilityHostConfig config;
    config.backend = AccessibilityBackend::Uia;
    config.report = [&log](std::string_view line) { log.add(std::string(line)); };
    config.create_uia = [harness]() -> std::unique_ptr<UiaHost> {
        auto apartment = std::make_unique<LoggingApartment>(harness->log);
        harness->apartment = apartment.get();
        auto platform =
            std::make_unique<FakePlatform>(harness->log, reinterpret_cast<void*>(0x1234));
        harness->platform = platform.get();
        return std::make_unique<UiaHost>(test_config(), std::move(apartment), std::move(platform));
    };
    return config;
}

// A UIA host whose start fails deterministically on every platform: no platform seam at all, so
// `UiaHost::start()` reports UnsupportedPlatform before any thread is spawned.
inline AccessibilityHostConfig unsupported_uia_config(Log& log) {
    AccessibilityHostConfig config;
    config.backend = AccessibilityBackend::Uia;
    config.report = [&log](std::string_view line) { log.add(std::string(line)); };
    config.create_uia = []() -> std::unique_ptr<UiaHost> { return nullptr; };
    return config;
}

// The Linux AT-SPI cases live in app_accessibility_host_atspi_tests.cpp: a synthetic bus peer on
// the fixture's loopback transport, so the real `AtspiSession` handshake runs against replayed
// daemon bytes. A failure at auth or a peer close must turn accessibility off with exactly one
// diagnostic, never a throw.
[[nodiscard]] bool atspi_handshake_starts_on_the_worker();
[[nodiscard]] bool atspi_auth_rejected_turns_accessibility_off();
[[nodiscard]] bool atspi_closed_peer_turns_accessibility_off();

} // namespace app_a11y_host_test
