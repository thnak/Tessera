// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"

#include <cstdint>
#include <memory>

namespace tessera::hal {

// The normalized description of one compositable surface: physical pixels, the scale the owner laid
// the surface out at, and the window-system family it belongs to. It carries no handle and no
// swapchain, so a renderer can size and validate a target from it while the rest of the engine
// (styles, layout, a component) can read it without ever touching the OS (ADR-020 rule 1).
//
// It is a snapshot, like `WindowMetrics`: the platform owner re-borrows the surface after a resize
// instead of mutating a borrowed one, so a renderer never observes a half-updated geometry.
struct SurfaceDesc {
    // Physical pixels of the surface's drawable area. Zero is the "no surface" value.
    std::uint32_t width_px = 0;
    std::uint32_t height_px = 0;
    // Device scale the surface is presented at (1.0 = one CSS px per device px).
    float dpi_scale = 1.0f;
    // The family the borrowed handles below belong to. `NoWindow` means the surface has no native
    // presentation target (a headless host): it is still renderable offscreen, which is what makes
    // the type testable without a GPU or a display.
    WindowSystem window_system = WindowSystem::NoWindow;

    // A surface is compositable when it has a drawable extent. The window-system family is not part
    // of validity: an offscreen surface has no native target and is still a surface.
    [[nodiscard]] bool valid() const noexcept {
        return width_px != 0 && height_px != 0;
    }

    friend bool operator==(const SurfaceDesc&, const SurfaceDesc&) = default;
};

// Liveness record of the object that owns the native handles (the platform window). It is shared by
// the owner and *weakly* observed by every `NativeSurface` borrowed from it, which is the whole
// ownership rule: a surface can tell whether the window still exists, and can never keep it alive.
//
// `alive` is deliberately not atomic: it is read and written on the window's thread only, the same
// rule `hal::Window` states for every method that touches a handle. A surface crossing threads
// would be a new contract, not an implementation detail.
struct SurfaceLiveness {
    bool alive = true;
};

// The owner side of that rule. A platform window (or a deterministic fake in a test) owns one
// `SurfaceLifetime` for as long as its native handles exist; every `NativeSurface` borrowed from it
// is a view, never an owner.
//
// Destroying the lifetime marks every surface borrowed from it invalid. `revoke()` does the same
// explicitly, for an owner that destroys its handles before it is itself destroyed (a platform
// window's explicit close, a surface lost under it).
class SurfaceLifetime {
public:
    SurfaceLifetime();
    ~SurfaceLifetime();

    // The owner and its surfaces are one per window: a copy would create a second liveness record
    // for the same handles, so the two would disagree about validity.
    SurfaceLifetime(const SurfaceLifetime&) = delete;
    SurfaceLifetime& operator=(const SurfaceLifetime&) = delete;
    SurfaceLifetime(SurfaceLifetime&&) = delete;
    SurfaceLifetime& operator=(SurfaceLifetime&&) = delete;

    // Ends every surface borrowed from this lifetime. Idempotent.
    void revoke() noexcept;
    [[nodiscard]] bool alive() const noexcept;

    // The witness a `NativeSurface` holds. Expired means "the owner was destroyed"; not-expired but
    // `alive() == false` means it was revoked explicitly. Both answer invalid on the surface side.
    [[nodiscard]] std::weak_ptr<const SurfaceLiveness> witness() const noexcept;

private:
    std::shared_ptr<SurfaceLiveness> liveness_;
};

// A ref-counted handle to a surface the *caller* owns, which a renderer composites into.
//
// Ownership rule - the reason this type exists instead of passing raw handles around: **a
// `NativeSurface` never owns a window handle.** It borrows the two opaque pointers a HAL window
// reports, keeps a weak witness of the owner's `SurfaceLifetime`, and reports invalid - with zeroed
// handles - the moment that owner is gone or revoked. It is ref-counted only over its *description*
// (copies share one immutable record), so a renderer can hold one across frames without extending
// the window's lifetime and without a use-after-free if the window is destroyed first.
//
// Everything here runs on the thread that owns the window. UI code must not call `native_handle()`
// (ADR-020 rule 1: UI sees `WindowMetrics`, never handles); only renderer/swapchain glue does,
// exactly as with `hal::Window::native_handle()`.
class NativeSurface final {
public:
    // No surface: every query answers invalid. The default value, so a host can hold one before a
    // window exists.
    NativeSurface() noexcept = default;

    // Borrows `handles` (never adopts them) and records `desc`. `surface_id` is the owner's stable
    // name for the surface; it is what a non-owning `NativeSurfaceRef` (ADR-050 section 7 item 1: a
    // node id plus generation) will compare against to detect a stale reference. 0 means "unnamed".
    //
    // The caller must guarantee that `handles` belong to `owner` and stay valid exactly as long as
    // `owner` reports them: this call copies the pointer values, so a handle the platform replaced
    // in place needs a fresh `borrow()` (a resize or a recreate re-borrows; it never mutates this
    // one). Not `noexcept`: it allocates the one shared record a set of copies shares.
    [[nodiscard]] static NativeSurface borrow(const SurfaceLifetime& owner, const SurfaceDesc& desc,
                                              NativeWindowHandle handles = {},
                                              std::uint64_t surface_id = 0);

    // True while the owner exists, has not revoked, and the description is usable. False never
    // means "dangling": the handles are unreadable through this type from that point on.
    [[nodiscard]] bool valid() const noexcept;

    // The frozen description, or a zeroed one when invalid.
    [[nodiscard]] SurfaceDesc desc() const noexcept;
    [[nodiscard]] std::uint64_t id() const noexcept;

    // Renderer glue only. Zeroed when the surface is invalid, so a caller that ignored `valid()`
    // still cannot dereference a handle the platform already destroyed.
    [[nodiscard]] NativeWindowHandle native_handle() const noexcept;

    // No `operator==`: two borrowed views name the same surface when they share an owner and a
    // non-zero `id()`, and `id()` already answers that. A defaulted comparison would test the
    // ref-count pointer instead, so two surfaces borrowed from one window would look unequal while
    // two borrowed from different windows could look equal.

private:
    struct State;

    explicit NativeSurface(std::shared_ptr<const State> state) noexcept;

    std::shared_ptr<const State> state_;
};

} // namespace tessera::hal
