// SPDX-License-Identifier: MIT
//
// The Linux half of the WindowHost accessibility controller (issue #186): an in-tree AT-SPI2 bridge
// plus its handshake/pump session. Everything here is platform-neutral C++ (the D-Bus client is
// hand-rolled, no libatspi/glib), so the same translation unit compiles and is tested on Windows
// against the loopback transport fixture.
//
// Threading (ADR-021): `AtspiSession::start()` spawns exactly one worker thread that owns all bus
// I/O. The UI thread must never pump: it only polls the session's atomic phase and copies bounded
// snapshots/actions through the bridge's existing mutex-protected seams. That is why this file has
// no `pump`/`pump_sync` call on the tick path at all - the worker pumps, and a handshake failure
// simply appears as `SessionPhase::Failed` on a later tick, which turns accessibility off.

#include "accessibility_host_impl.hpp"

#include <array>
#include <utility>

namespace tessera::app {
namespace {

namespace a11y = tessera::ui::a11y;

} // namespace

bool AccessibilityHost::Impl::start_atspi() {
    status.backend = AccessibilityBackend::Atspi;
    bridge = std::make_unique<a11y::AtspiBridge>();
    if (!config.create_atspi) {
        config.create_atspi = [](a11y::AtspiBridge& target) {
            return std::make_unique<a11y::AtspiSession>(target,
                                                        a11y::session_config_from_environment());
        };
    }
    session = config.create_atspi(*bridge);
    if (session == nullptr) {
        disable("atspi", "no AT-SPI transport on this build");
        return false;
    }
    // Non-blocking: this only spawns the worker thread. The handshake itself runs on that thread;
    // a failure is observed by a later tick() through the session's phase, never by waiting here.
    const auto started = session->start();
    if (!started) {
        const std::string reason(to_string(started.error()));
        disable("atspi", reason);
        return false;
    }
    status.enabled = true;
    dirty = true;
    return true;
}

bool AccessibilityHost::Impl::tick_atspi() {
    if (bridge == nullptr || session == nullptr) {
        return false;
    }
    // The handshake and the connection are the worker's: report a terminal phase as one diagnostic
    // and turn accessibility off. A `Closed` session (the registry went away) is the same outcome -
    // there is no reconnect in this slice, and the window must stay functional either way.
    const a11y::SessionPhase phase = session->phase();
    if (phase == a11y::SessionPhase::Failed) {
        disable("atspi", a11y::to_string(session->error()));
        return false;
    }
    if (phase == a11y::SessionPhase::Closed) {
        disable("atspi", "session closed");
        return false;
    }

    bool published = false;
    if (dirty) {
        // Build on the UI thread; the bridge copies the bounded tree into its own retained buffer
        // under a short mutex and the worker serves it on the accessibility bus.
        try {
            build(snapshot);
        } catch (...) {
            // Boundary: an untrusted snapshot source. Keep the previous tree published and stay
            // dirty so a later tick retries; report it as a value (zero publishes this tick).
            ++status.failed_builds;
            return false;
        }
        bridge->publish(snapshot);
        status.nodes = snapshot.nodes.size();
        ++status.publishes;
        dirty = false;
        published = true;
    }

    // Tick-boundary action drain, bounded per tick: assistive technology validates and queues the
    // request; the window driver executes it here, on the UI thread, in arrival order.
    std::array<a11y::ActionRequest, a11y::kMaxPendingActions> batch{};
    const std::size_t taken = bridge->take_actions(batch);
    if (taken > 0) {
        for (std::size_t index = 0; index < taken; ++index) {
            AccessibilityAction action;
            map_action(batch[index].node, batch[index].action, action);
            if (action.node != 0) {
                apply_actions(std::span<const AccessibilityAction>(&action, 1));
                ++status.actions;
            }
        }
    }
    return published;
}

} // namespace tessera::app
