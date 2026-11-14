// SPDX-License-Identifier: MIT
//
// Platform-neutral accessibility controller owned by WindowHost (issues #185 and #186,
// docs/specs/accessibility-v1.md section 12.3). It owns exactly one backend per window:
//
// - Windows: a `UiaHost`, which installs `make_window_root_provider(UiaHost&)` on the HAL window
//   after forwarding `hal::Window::accessibility_host_handle()` to the platform seam, so
//   WM_GETOBJECT reaches the bridge-thread root provider.
// - Linux: an `AtspiBridge` + `AtspiSession`, whose own worker thread runs the AT-SPI2 handshake
//   and pumps the accessibility bus.
//
// In both cases the controller is the only place that turns a failure into behaviour: a failed or
// unsupported start (and a terminal session/bridge failure) turns accessibility off, leaves the
// window fully functional, and reports exactly one `TESSERA_A11Y_UNSUPPORTED:` diagnostic. Nothing
// here throws, and the UI thread never waits on a bridge or a bus.
//
// Internal to the app window library: `include/tessera/app/` does not expose this type.

#pragma once

#include "tessera/hal/window.hpp"
#include "tessera/ui/accessibility/a11y_tree.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace tessera::ui::a11y {
class AtspiBridge;
class AtspiSession;
class UiaHost;
} // namespace tessera::ui::a11y

namespace tessera::app {

// Which platform bridge a controller drives. `Auto` is the production choice: UIA on Windows and
// AT-SPI everywhere else. The explicit values exist for tests and diagnostics.
enum class AccessibilityBackend : std::uint8_t {
    Auto = 0,
    None,
    Uia,
    Atspi,
};

[[nodiscard]] std::string_view to_string(AccessibilityBackend backend);

// One validated accessibility action that arrived from assistive technology, resolved to a scene
// node id. The controller drains a bounded batch at a tick boundary; the window host executes it.
// The action mask is the semantics snapshot's own (`Action`), so no UIA/AT-SPI type reaches the
// window driver.
struct AccessibilityAction {
    ui::NodeId node = 0;
    ui::a11y::Action action = ui::a11y::Action::None;
};

// Builds the current semantics snapshot into a caller-owned buffer, reusing its capacity. Called on
// the UI thread and only when the controller is dirty, so a static window never builds a tree. A
// throwing source is caught at the boundary and reported as a value; the previous tree stays
// published.
using AccessibilitySnapshotSource = std::function<void(ui::a11y::A11ySnapshot&)>;
// Executes one drained action on the UI thread at a tick boundary. A throwing handler ends that
// tick's batch; the action is dropped, never retried.
using AccessibilityActionHandler = std::function<void(const AccessibilityAction&)>;

struct AccessibilityHostConfig {
    AccessibilityBackend backend = AccessibilityBackend::Auto;
    // One-line diagnostic sink; null writes to stderr. Tests capture the single
    // TESSERA_A11Y_UNSUPPORTED line here instead of scraping the process's stderr.
    std::function<void(std::string_view line)> report;
    // Test seams. Null uses the production platform selection for the chosen backend: a `UiaHost`
    // over `create_platform_uia()`/`create_platform_apartment()`, or an `AtspiSession` over the
    // UNIX-socket transport provider with the environment's bus address. A UIA factory that
    // returns nullptr models a build with no UIA platform seam (start reports UnsupportedPlatform).
    std::function<std::unique_ptr<ui::a11y::UiaHost>()> create_uia;
    std::function<std::unique_ptr<ui::a11y::AtspiSession>(ui::a11y::AtspiBridge&)> create_atspi;
};

// Structured state for tests and diagnostics. `error` is the diagnostic's reason text (the error
// name) and stays empty while accessibility is healthy.
struct AccessibilityHostStatus {
    bool enabled = false;
    bool failed = false;
    AccessibilityBackend backend = AccessibilityBackend::None;
    std::string error;
    // Snapshot builds, publishes to a bridge, drained actions, and the last published node count.
    // A static window keeps every counter at its last value. `failed_builds` counts a throwing
    // snapshot source (the previous tree stays published) and `failed_actions` a throwing action
    // handler (that action is dropped, not retried).
    std::uint64_t snapshots = 0;
    std::uint64_t publishes = 0;
    std::uint64_t actions = 0;
    std::uint64_t failed_builds = 0;
    std::uint64_t failed_actions = 0;
    std::size_t nodes = 0;
};

class AccessibilityHost {
public:
    AccessibilityHost();
    ~AccessibilityHost();

    AccessibilityHost(const AccessibilityHost&) = delete;
    AccessibilityHost& operator=(const AccessibilityHost&) = delete;

    // Installs the root provider (UIA) and starts the backend. `window` and the callbacks must
    // outlive the controller or `stop()` must run first. Returns false when accessibility is off;
    // the failure has already been reported (at most one diagnostic) and nothing is left installed.
    // Never throws and never waits for a bridge thread or a bus.
    bool start(hal::Window& window, AccessibilitySnapshotSource source,
               AccessibilityActionHandler handler = {},
               AccessibilityHostConfig config = {});

    // Uninstalls the installed seam and joins/stops the backend. Safe twice, from the destructor,
    // and after a failed start. The caller must invoke it while the platform window is still valid.
    void stop();

    // Marks the semantics snapshot dirty. Called from the style/layout/focus/resize/input paths,
    // never per frame.
    void mark_dirty();
    [[nodiscard]] bool dirty() const;

    // UI thread, once per driver iteration: publishes a snapshot only when dirty (or when the
    // client area changed), then drains a bounded action batch. Returns true when it published.
    // Never blocks: the UIA host only touches a bounded handoff, and the AT-SPI worker thread owns
    // all bus I/O.
    bool tick();

    [[nodiscard]] const AccessibilityHostStatus& status() const;
    [[nodiscard]] bool enabled() const;

    // Test seams: the live backend objects (null when that backend is not the active one). Not part
    // of the runtime contract; the tests use them to inspect the bridge/host the controller built
    // and to drive the session's worker with its deterministic `pump_sync` barrier.
    [[nodiscard]] ui::a11y::UiaHost* uia_host_for_testing();
    [[nodiscard]] ui::a11y::AtspiBridge* atspi_bridge_for_testing();
    [[nodiscard]] ui::a11y::AtspiSession* atspi_session_for_testing();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::app
