// SPDX-License-Identifier: MIT
//
// The Linux AT-SPI half of the `app_accessibility_host` test (issue #186): a synthetic bus peer on
// the loopback transport, so the controller drives the real `AtspiSession` handshake against
// replayed daemon bytes. No test needs a bus daemon, a socket or a screen reader. The shared helper
// declarations live in `app_accessibility_host_fixture.hpp`; the Windows UIA half and `main` are in
// `app_accessibility_host_tests.cpp`, so both halves compile and run on every platform.

#include "app_accessibility_host_fixture.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace app_a11y_host_test {
namespace {

using a11y_dbus_test::bytes_of;
using a11y_session_test::Harness;
using tessera::ui::a11y::AtspiBridge;
using tessera::ui::a11y::AtspiSession;

AccessibilityHostConfig atspi_config(std::shared_ptr<Harness> harness, Log& log) {
    AccessibilityHostConfig config;
    config.backend = AccessibilityBackend::Atspi;
    config.report = [&log](std::string_view line) { log.add(std::string(line)); };
    config.create_atspi = [harness](AtspiBridge& bridge) {
        return std::make_unique<AtspiSession>(bridge, harness->provider, Harness::make_config());
    };
    return config;
}

} // namespace

// The session starts, the worker connects and writes the SASL request, and the controller stays
// enabled while the handshake is in flight. `pump_sync` is the fixture's deterministic barrier on
// that worker; it is called here from the test thread, never from a host's tick path.
bool atspi_handshake_starts_on_the_worker() {
    auto harness = std::make_shared<Harness>();
    Log log;
    FakeWindow window;
    AccessibilityHost controller;
    if (!expect(controller.start(window, [](A11ySnapshot& out) { build_button_tree(out); }, {},
                                 atspi_config(harness, log)),
                "the AT-SPI controller starts") ||
        !expect(controller.enabled(), "accessibility is enabled while handshaking") ||
        !expect(controller.atspi_session_for_testing() != nullptr,
                "the controller owns the session")) {
        return false;
    }
    (void)controller.atspi_session_for_testing()->pump_sync(0);
    const std::vector<std::uint8_t> written =
        a11y_session_test::read_output(harness->provider.peer(0));
    return expect(!written.empty(), "the worker wrote the SASL request") &&
           expect(log.lines.empty(), "a healthy handshake reports nothing") &&
           expect(controller.enabled(), "a handshaking session keeps accessibility on");
}

// A rejected SASL identity fails the handshake: one diagnostic, accessibility off, and the
// controller stays inert and throw-free afterwards.
bool atspi_auth_rejected_turns_accessibility_off() {
    auto harness = std::make_shared<Harness>();
    Log log;
    FakeWindow window;
    AccessibilityHost controller;
    if (!expect(controller.start(window, [](A11ySnapshot& out) { build_button_tree(out); }, {},
                                 atspi_config(harness, log)),
                "the AT-SPI controller starts")) {
        return false;
    }
    // Drive the worker one step: it writes SASL EXTERNAL on peer 0.
    (void)controller.atspi_session_for_testing()->pump_sync(0);
    (void)a11y_session_test::read_output(harness->provider.peer(0));
    // The refusal needs no D-Bus framing: the peer answers the SASL line directly.
    (void)harness->provider.peer(0).write_all(bytes_of("REJECTED EXTERNAL\r\n"));
    (void)controller.atspi_session_for_testing()->pump_sync(0);
    // The controller observes the terminal phase on its next tick and disables itself.
    (void)controller.tick();
    if (!expect(!controller.enabled(), "a rejected handshake turns accessibility off") ||
        !expect(controller.status().failed, "the status reports the failure") ||
        !expect(log.lines.size() == 1, "exactly one diagnostic was reported")) {
        return false;
    }
    const std::string& line = log.lines[0];
    return expect(line.rfind("TESSERA_A11Y_UNSUPPORTED:", 0) == 0,
                  "the diagnostic uses the documented prefix") &&
           expect(line.find("atspi") != std::string::npos, "the diagnostic names the backend") &&
           expect(line.find("auth rejected") != std::string::npos,
                  "the diagnostic carries the session error name") &&
           expect(!window.accessibility_root_installed(),
                  "the AT-SPI path installs no window root") &&
           expect(!controller.tick(), "a disabled controller stays inert") &&
           expect(log.lines.size() == 1, "later ticks do not report again");
}

// A peer that closes before Ready is the second failure shape; the diagnostic is one line and the
// controller never throws.
bool atspi_closed_peer_turns_accessibility_off() {
    auto harness = std::make_shared<Harness>();
    Log log;
    FakeWindow window;
    AccessibilityHost controller;
    if (!expect(controller.start(window, [](A11ySnapshot& out) { build_button_tree(out); }, {},
                                 atspi_config(harness, log)),
                "the AT-SPI controller starts")) {
        return false;
    }
    (void)controller.atspi_session_for_testing()->pump_sync(0);
    (void)a11y_session_test::read_output(harness->provider.peer(0));
    harness->provider.peer(0).shutdown();
    (void)controller.atspi_session_for_testing()->pump_sync(0);
    bool threw = false;
    try {
        (void)controller.tick();
    } catch (...) {
        threw = true;
    }
    return expect(!threw, "a closed peer does not throw") &&
           expect(!controller.enabled(), "a closed peer turns accessibility off") &&
           expect(log.lines.size() == 1, "the failure is reported exactly once");
}

} // namespace app_a11y_host_test
