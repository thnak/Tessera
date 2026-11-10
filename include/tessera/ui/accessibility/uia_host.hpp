// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/accessibility/uia_bridge.hpp"

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>

namespace tessera::ui::a11y {

// The WM_GETOBJECT lParam a UI Automation client uses to ask a window for its root provider.
// Windows spells it `UiaRootObjectId`; the value is repeated here so this header stays SDK-free and
// every platform compiles it (`uia_sdk_probe.cpp` static_asserts it against the SDK on Windows).
inline constexpr std::intptr_t kUiaRootObjectId = -25;

// Platform seam for the host wiring (docs/specs/accessibility-v1.md section 12.2). The Windows
// implementation creates the COM root provider and raises UIA events with `UIAutomationCore`; every
// other platform has no implementation, so `create_platform_uia()` returns nullptr and the host
// reports accessibility disabled instead of failing the app.
class UiaPlatform {
public:
    virtual ~UiaPlatform() = default;

    // Bridge thread: runs after the COM apartment is initialized and before the bridge reports
    // Running, so the root object is created on the thread that owns it. Returns the raw provider
    // pointer the window hands to `UiaReturnRawElementProvider`, or nullptr when accessibility
    // stays off.
    [[nodiscard]] virtual void* create_root(UiaBridge& bridge) = 0;
    // Bridge thread: releases the root provider before the apartment shuts down. Must tolerate a
    // failed or absent `create_root`.
    virtual void destroy_root() = 0;
};

// Windows: a `UiaPlatform` over the UIA provider surface. Every other platform: nullptr.
[[nodiscard]] std::unique_ptr<UiaPlatform> create_platform_uia();

struct UiaHostConfig {
    UiaBridgeConfig bridge;
};

// The window/host owner of the UIA bridge: the piece that connects a Win32 window's WM_GETOBJECT to
// the bridge thread's provider. It never blocks the UI/window thread and never calls UIA there.
//
// Handshake: `start()` only spawns the bridge. The bridge thread initializes the MTA apartment and
// then runs the start hook, which creates the root provider and publishes its raw pointer in an
// atomic. Until that pointer is published (starting, failed, stopped, or no platform), the window
// answers `false` and falls back to `DefWindowProc`, so UIA sees the default MSAA bridge instead of
// an empty provider. `stop()` clears the pointer before the bridge releases the object.
//
// Threads: `start`, `stop`, and `answer_get_object` run on the window's thread (the thread that
// dispatches WM_GETOBJECT); the hooks run on the bridge thread. Nothing here waits for the bridge.
class UiaHost {
public:
    // `apartment` overrides the platform seam (tests inject a fake COM apartment); `platform`
    // overrides the UIA root seam (tests inject a fake root/raiser).
    explicit UiaHost(UiaHostConfig config = {}, std::unique_ptr<UiaApartment> apartment = {},
                     std::unique_ptr<UiaPlatform> platform = {});
    ~UiaHost();

    UiaHost(const UiaHost&) = delete;
    UiaHost& operator=(const UiaHost&) = delete;

    // Non-blocking: spawns the bridge thread. Returns `UnsupportedPlatform` when this build has no
    // UIA platform seam (an expected, diagnostic-only outcome), or the bridge's start error.
    [[nodiscard]] std::expected<void, UiaError> start();
    // Clears the root pointer and joins the bridge; safe twice, from the destructor, or when never
    // started.
    void stop();

    // True once the bridge thread published a root provider. The window thread only reads this.
    [[nodiscard]] bool ready() const;
    // Window thread, inside message dispatch: the answer for one WM_GETOBJECT. Never blocks, never
    // calls UIA. Returns true only for the UIA root object id while a root is published; false
    // means the caller must fall back to the platform default.
    [[nodiscard]] bool answer_get_object(std::intptr_t object_id, void*& raw_provider) const;

    [[nodiscard]] UiaBridge& bridge();
    [[nodiscard]] const UiaBridge& bridge() const;
    [[nodiscard]] UiaError error() const;

private:
    void on_bridge_start(UiaProvider& provider);
    void on_bridge_stop(UiaProvider& provider);

    UiaHostConfig config_;
    std::unique_ptr<UiaPlatform> platform_;
    UiaBridge bridge_;
    // Published by the bridge thread once the root exists and cleared before it is released.
    std::atomic<void*> root_{nullptr};
};

} // namespace tessera::ui::a11y
