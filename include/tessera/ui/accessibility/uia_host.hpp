// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"
#include "tessera/ui/accessibility/uia_bridge.hpp"
#include "tessera/ui/accessibility/uia_events.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>

namespace tessera::ui::a11y {

// The WM_GETOBJECT lParam a UI Automation client uses to ask a window for its root provider.
// Windows spells it `UiaRootObjectId`; the value is repeated here so this header stays SDK-free and
// every platform compiles it (`uia_sdk_probe.cpp` static_asserts it against the SDK on Windows).
inline constexpr std::intptr_t kUiaRootObjectId = -25;

// Host-element wiring state for the root provider (`get_HostRawElementProvider`). The root answers
// E_FAIL until a native handle is installed and a host provider was built from it; the state is
// latched on failure so a broken window cannot turn every client query into another COM attempt.
enum class UiaHostElementState : std::uint8_t {
    Unavailable, // no native host handle is installed (or this platform has none)
    Pending,     // a handle is installed; the host provider is built on the first UIA query
    Ready,       // the root answered with a `UiaHostProviderFromHwnd` provider
    Failed,      // an installed handle produced no host provider; latched until the handle changes
};

struct UiaHostElementStatus {
    UiaHostElementState state = UiaHostElementState::Unavailable;
    bool handle_installed = false;
    // Failed `UiaHostProviderFromHwnd` attempts; never incremented more than once per installed
    // handle, so a caller can report it without log spam.
    std::uint64_t failures = 0;
};

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
    // Bridge thread: raises one coalesced batch of UIA events. Called only from a posted bridge
    // job, never from the window thread. A raised error drops the batch and is reported as a value
    // (the tree itself is already published, so clients re-query on the next structure event).
    [[nodiscard]] virtual UiaError raise(std::span<const UiaEvent> events) = 0;

    // Window thread, before or after `start`: installs the opaque native host element the root
    // answers `get_HostRawElementProvider` from. The platform only stores the token; the
    // `UiaHostProviderFromHwnd` call happens on the bridge thread on demand, so this never calls
    // COM and never blocks. Idempotent; a null handle clears the wiring. A handle installed before
    // `start` is the normal path.
    virtual void set_host_handle(hal::AccessibilityHostHandle handle) {
        (void)handle;
    }
    // Any thread: structured state of the host-element wiring, for one diagnostic instead of one
    // line per client call. Never calls COM.
    [[nodiscard]] virtual UiaHostElementStatus host_element_status() const {
        return {};
    }
};

// Windows: a `UiaPlatform` over the UIA provider surface. Every other platform: nullptr.
[[nodiscard]] std::unique_ptr<UiaPlatform> create_platform_uia();

struct UiaHostConfig {
    UiaBridgeConfig bridge;
    // Actions one tick may execute. The rest stay queued (bounded by the provider ring) for the
    // next tick, so a spamming client cannot stretch a frame.
    std::size_t max_actions_per_tick = 8;
};

// What one tick did. A static window (not dirty, no queued actions) reports zeroes and does no
// snapshot build, no publish, no bridge wake, and no UIA call.
struct UiaHostTickStats {
    bool published = false;           // a dirty snapshot was built and published
    bool build_failed = false;        // the source threw; the previous tree is still served
    std::size_t events_queued = 0;    // coalesced events this tick contributed
    std::size_t events_pending = 0;   // events still waiting for a bridge flush
    std::size_t actions_applied = 0;  // actions executed in this tick
    std::size_t actions_failed = 0;   // the handler threw (that action is dropped, not retried)
    std::size_t actions_deferred = 0; // actions still queued for a later tick
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
    // UI thread: builds the semantics snapshot into a caller-owned buffer. Called only when the
    // host is dirty, so a static window never builds a tree.
    using SnapshotSource = std::function<void(A11ySnapshot&)>;
    // UI thread, tick boundary: executes one validated accessibility action. Never called on the
    // bridge thread and never re-entrant with a UIA client call.
    using ActionHandler = std::function<void(const UiaActionRequest&)>;

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

    // Window thread: installs the window's opaque native host element so the root can answer
    // `get_HostRawElementProvider`. Call it before `start()` (the usual path) or while running;
    // repeated or identical installs are free. Never calls COM and never blocks. A missing handle
    // makes the root fail that one query cleanly instead of answering with a providerless element.
    void set_host_handle(hal::AccessibilityHostHandle handle);
    // Any thread: structured diagnostic for the host-element wiring. Reads one atomic snapshot and
    // never calls COM; a failed build stays latched until a new handle is installed.
    [[nodiscard]] UiaHostElementStatus host_element_status() const;

    // UI thread: marks the semantics snapshot dirty. Call it from the style/layout/focus/text
    // change paths, not per frame; repeated calls before a tick are free.
    void mark_dirty();
    [[nodiscard]] bool dirty() const;

    // UI thread, tick boundary: publishes a snapshot only when dirty (deriving the coalesced UIA
    // events from the tree diff), asks the bridge thread to raise them, then drains and applies
    // queued accessibility actions in arrival order. A null source publishes an empty tree; a null
    // handler leaves actions queued instead of discarding them. Never blocks the tick.
    [[nodiscard]] UiaHostTickStats tick(const SnapshotSource& source,
                                        const ActionHandler& handler = {});

    [[nodiscard]] UiaBridge& bridge();
    [[nodiscard]] const UiaBridge& bridge() const;
    [[nodiscard]] UiaError error() const;

    // Diagnostics: bridge-thread flushes that raised a batch, and flushes the platform refused.
    [[nodiscard]] std::uint64_t raised_batches() const;
    [[nodiscard]] std::uint64_t failed_raises() const;
    [[nodiscard]] std::size_t pending_events() const;

private:
    void on_bridge_start(UiaProvider& provider);
    void on_bridge_stop(UiaProvider& provider);
    // Bridge thread: raise everything currently queued, then clear the flush ticket.
    void flush_events();

    UiaHostConfig config_;
    std::unique_ptr<UiaPlatform> platform_;
    UiaBridge bridge_;
    // Published by the bridge thread once the root exists and cleared before it is released.
    std::atomic<void*> root_{nullptr};
    std::atomic<bool> dirty_{false};
    // Exactly one flush job may be in flight; a tick never queues a second one.
    std::atomic<bool> flush_posted_{false};
    std::atomic<std::uint64_t> raised_batches_{0};
    std::atomic<std::uint64_t> failed_raises_{0};
    UiaEventQueue events_;
    // Reused build/retain buffers so a steady-state publish allocates nothing after the first.
    A11ySnapshot next_;
    A11ySnapshot previous_;
};

// The WM_GETOBJECT answer for one window: forwards to `host.answer_get_object` on the window's
// thread, where it is one atomic load. Install it with `hal::Window::set_accessibility_root`; the
// host must outlive the window or the root must be uninstalled first. Calling it again for the
// same host is idempotent (the callback and context are the same).
[[nodiscard]] hal::AccessibilityRoot make_window_root_provider(UiaHost& host);

} // namespace tessera::ui::a11y
