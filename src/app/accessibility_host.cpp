// SPDX-License-Identifier: MIT
//
// Platform-neutral accessibility controller owned by WindowHost (issues #185 and #186). This file
// owns the lifecycle, the Windows UIA wiring and the dirty-driven tick; the Linux AT-SPI half lives
// in accessibility_host_atspi.cpp so each translation unit stays small. See accessibility_host.hpp
// for the contract.

#include "accessibility_host.hpp"

#include "accessibility_host_impl.hpp"

#include <cstdio>
#include <utility>

namespace tessera::app {
namespace {

namespace a11y = tessera::ui::a11y;

bool windows_backend() {
#if defined(_WIN32)
    return true;
#else
    return false;
#endif
}

void default_report(std::string_view line) {
    std::fprintf(stderr, "%.*s\n", static_cast<int>(line.size()), line.data());
}

} // namespace

std::string_view to_string(AccessibilityBackend backend) {
    switch (backend) {
    case AccessibilityBackend::Auto:
        return "auto";
    case AccessibilityBackend::None:
        return "none";
    case AccessibilityBackend::Uia:
        return "uia";
    case AccessibilityBackend::Atspi:
        return "atspi";
    }
    return "auto";
}

bool AccessibilityHost::Impl::start(hal::Window& target_window,
                                    AccessibilitySnapshotSource snapshot_source,
                                    AccessibilityActionHandler action_handler,
                                    AccessibilityHostConfig host_config) {
    teardown();
    window = &target_window;
    source = std::move(snapshot_source);
    handler = std::move(action_handler);
    config = std::move(host_config);
    if (!config.report) {
        config.report = &default_report;
    }
    status = AccessibilityHostStatus{};
    diagnostic_reported = false;
    dirty = false;
    const hal::WindowMetrics metrics = window->metrics();
    width = metrics.logical_width;
    height = metrics.logical_height;

    switch (config.backend) {
    case AccessibilityBackend::None:
        status.backend = AccessibilityBackend::None;
        return false;
    case AccessibilityBackend::Auto:
        return windows_backend() ? start_uia() : start_atspi();
    case AccessibilityBackend::Uia:
        return start_uia();
    case AccessibilityBackend::Atspi:
        return start_atspi();
    }
    return false;
}

bool AccessibilityHost::Impl::start_uia() {
    status.backend = AccessibilityBackend::Uia;
    if (!config.create_uia) {
        // Production: the host's own platform selection. On a build without the UIA seam
        // (`create_platform_uia()` returns nullptr) `start()` reports UnsupportedPlatform, which is
        // the documented "accessibility off" path, not an app failure.
        config.create_uia = [] { return std::make_unique<a11y::UiaHost>(); };
    }
    uia = config.create_uia();
    if (uia == nullptr) {
        // A build with no UIA seam: `UiaHost::start()` would report the same value, so a injected
        // factory that cannot build a host is the same documented "accessibility off" outcome.
        disable("uia", to_string(a11y::UiaError::UnsupportedPlatform));
        return false;
    }
    // Order matters only for readability: the opaque handle may be installed before the bridge
    // starts, and the root answer is one atomic load that stays "not ready" until the bridge
    // publishes. `hal::Window::accessibility_host_handle()` is a null token on every platform
    // without a native host element (X11), which the platform seam treats as "no host element",
    // not as an error.
    uia->set_host_handle(window->accessibility_host_handle());
    window->set_accessibility_root(a11y::make_window_root_provider(*uia));
    root_installed = true;
    const auto started = uia->start();
    if (!started) {
        // Failures are values: uninstall the answer so the window keeps its default accessibility
        // provider, report one diagnostic, and leave the window fully functional.
        const std::string reason(to_string(started.error()));
        teardown();
        disable("uia", reason);
        return false;
    }
    status.enabled = true;
    // The first tree must reach the bridge: the window already has content by the time the
    // controller exists, and `UiaHost` is the dirty-flag owner on this backend.
    uia->mark_dirty();
    dirty = false;
    return true;
}

void AccessibilityHost::Impl::mark_dirty() {
    // UIA owns its own dirty flag (the bridge consumes it through `UiaHost::tick`); the AT-SPI
    // backend reuses this controller's flag, because nothing else in the app knows about it.
    if (uia != nullptr) {
        uia->mark_dirty();
        return;
    }
    dirty = true;
}

bool AccessibilityHost::Impl::is_dirty() const {
    return uia != nullptr ? uia->dirty() : dirty;
}

void AccessibilityHost::Impl::disable(std::string_view backend_name, std::string_view reason) {
    // Accessibility off is a value, not a failure of the app: one latched diagnostic line, the
    // backend dropped, the window untouched. A second failure (a retry, a late session error) never
    // reports again.
    teardown();
    status.enabled = false;
    status.failed = true;
    status.error.assign(reason);
    if (diagnostic_reported) {
        return;
    }
    diagnostic_reported = true;
    std::string line = "TESSERA_A11Y_UNSUPPORTED: ";
    line.append(backend_name);
    line.append(": ");
    line.append(reason);
    line.append("; accessibility is off and the window keeps running");
    if (config.report) {
        config.report(line);
    }
}

void AccessibilityHost::Impl::teardown() {
    // The root answer must be uninstalled while the platform window (and its native handle) is
    // still valid: the window procedure must never call into a destroyed controller. Both halves
    // are idempotent.
    if (root_installed && window != nullptr) {
        root_installed = false;
        window->set_accessibility_root({});
    }
    if (uia != nullptr) {
        uia->stop();
        uia.reset();
    }
    if (session != nullptr) {
        // Joins the session's one worker thread. This runs on the teardown path only, never on a
        // healthy per-frame path.
        session->stop();
        session.reset();
    }
    bridge.reset();
    status.enabled = false;
}

void AccessibilityHost::Impl::build(a11y::A11ySnapshot& out) {
    ++status.snapshots;
    if (source) {
        source(out);
    } else {
        out.clear();
    }
}

bool AccessibilityHost::Impl::tick() {
    if (!status.enabled) {
        return false;
    }
    // A size change is a dirty source on its own: the root bounds and every world rect change with
    // the client area, and a resize does not necessarily touch the scene's dirty flags.
    sync_size_dirty();
    if (status.backend == AccessibilityBackend::Uia) {
        return tick_uia();
    }
    if (status.backend == AccessibilityBackend::Atspi) {
        return tick_atspi();
    }
    return false;
}

bool AccessibilityHost::Impl::tick_uia() {
    if (uia == nullptr) {
        return false;
    }
    // `UiaHost::tick` builds only when dirty, publishes into a bounded one-slot handoff (never a
    // wait, never a UIA/COM call on this thread), raises the coalesced events on the bridge thread
    // and drains a bounded action batch. A throwing source keeps the previous tree published.
    bool built_now = false;
    const auto stats = uia->tick(
        [this, &built_now](a11y::A11ySnapshot& out) {
            build(out);
            // Keep the tree the host just built: the action drain needs index -> NodeId, and the
            // node count is the diagnostic. `assign` reuses the buffer's capacity.
            snapshot.nodes.assign(out.nodes.begin(), out.nodes.end());
            status.nodes = snapshot.nodes.size();
            built_now = true;
        },
        [this](const a11y::UiaActionRequest& request) {
            AccessibilityAction action;
            map_action(request.node, request.action, action);
            if (action.node != 0) {
                apply_actions(std::span<const AccessibilityAction>(&action, 1));
            }
        });
    dirty = uia->dirty();
    if (stats.published && built_now) {
        ++status.publishes;
    }
    status.actions += stats.actions_applied;
    return stats.published;
}

void AccessibilityHost::Impl::sync_size_dirty() {
    if (window == nullptr) {
        return;
    }
    const hal::WindowMetrics metrics = window->metrics();
    if (metrics.logical_width != width || metrics.logical_height != height) {
        width = metrics.logical_width;
        height = metrics.logical_height;
        mark_dirty();
    }
}

void AccessibilityHost::Impl::map_action(a11y::A11yIndex index, a11y::Action action,
                                         AccessibilityAction& out) {
    out = AccessibilityAction{};
    if (index == a11y::kInvalidIndex || index >= snapshot.nodes.size()) {
        // A stale request for a tree that has already been replaced: drop it, never dereference.
        return;
    }
    out.node = snapshot.nodes[index].id;
    out.action = action;
}

void AccessibilityHost::Impl::apply_actions(std::span<const AccessibilityAction> actions) {
    for (const AccessibilityAction& action : actions) {
        if (action.node == 0 || !handler) {
            continue;
        }
        try {
            handler(action);
        } catch (...) {
            // Boundary: an accessibility action runs host/app code. A throw ends this tick's batch;
            // the action is dropped (never retried) and the failure is counted, not thrown on.
            ++status.failed_actions;
            return;
        }
    }
}

} // namespace tessera::app
