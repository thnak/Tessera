// SPDX-License-Identifier: MIT
//
// Private state of `AccessibilityHost`, shared by the two translation units that implement it:
// `accessibility_host.cpp` (lifecycle, the Windows UIA path, the tick) and
// `accessibility_host_atspi.cpp` (the Linux AT-SPI session + its action drain). Not installed; only
// src/app includes it.

#pragma once

#include "accessibility_host.hpp"
#include "tessera/ui/accessibility/atspi_bridge.hpp"
#include "tessera/ui/accessibility/atspi_session.hpp"
#include "tessera/ui/accessibility/uia_host.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace tessera::app {

struct AccessibilityHost::Impl {
    Impl() = default;
    ~Impl() = default;

    bool start(hal::Window& window, AccessibilitySnapshotSource source,
               AccessibilityActionHandler handler, AccessibilityHostConfig config);
    void mark_dirty();
    [[nodiscard]] bool is_dirty() const;
    bool tick();

    // Starts one backend. Each returns false after reporting its own single diagnostic. The two
    // AT-SPI functions live in accessibility_host_atspi.cpp.
    bool start_uia();
    bool start_atspi();
    // One tick's work for the active backend. Returns true when a snapshot was published.
    bool tick_uia();
    bool tick_atspi();

    // Builds the semantics snapshot through the installed source and keeps it for the index ->
    // NodeId map an action drain needs. A throwing source is the caller's to catch.
    void build(ui::a11y::A11ySnapshot& out);
    // Reports one `TESSERA_A11Y_UNSUPPORTED: ...` line (latched: at most once per controller) and
    // turns accessibility off. The window keeps running.
    void disable(std::string_view backend_name, std::string_view reason);
    // The one teardown path: uninstalls the UIA root (the native handle must still be valid), then
    // stops and drops the backend. Safe twice.
    void teardown();
    // Turns one backend node index into the scene-side action the window driver executes.
    void map_action(ui::a11y::A11yIndex index, ui::a11y::Action action, AccessibilityAction& out);
    // Runs a drained batch on the UI thread; a throwing handler ends the batch.
    void apply_actions(std::span<const AccessibilityAction> actions);
    // Marks dirty when the client area changed since the last tick (the POC's width/height check).
    void sync_size_dirty();

    hal::Window* window = nullptr;
    // Exactly one of these is non-null while accessibility is enabled.
    std::unique_ptr<ui::a11y::UiaHost> uia;
    bool root_installed = false;
    std::unique_ptr<ui::a11y::AtspiBridge> bridge;
    std::unique_ptr<ui::a11y::AtspiSession> session;

    AccessibilitySnapshotSource source;
    AccessibilityActionHandler handler;
    AccessibilityHostConfig config;
    AccessibilityHostStatus status;
    // The last built tree: the index -> NodeId map for an action drain, and the node count.
    ui::a11y::A11ySnapshot snapshot;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The AT-SPI backend's dirty flag. The UIA backend's flag lives in `UiaHost` (it owns the
    // publication contract), so `mark_dirty`/`is_dirty` forward there when it is the active one.
    bool dirty = false;
    bool diagnostic_reported = false;
};

} // namespace tessera::app
