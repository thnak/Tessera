// SPDX-License-Identifier: MIT
//
// AF6 (issue #178, ADR-050 section 7): the ownership rule of `hal::NativeSurface`, which is the one
// thing the acceptance names - "the surface never owns window handles (the HAL boundary)".
//
// The rule is proved in both directions, against a fake window that records every teardown request:
//
//   * destroying the *surface* does not destroy the window: the fake's teardown counter stays zero,
//     its lifetime stays alive, and its handle values are untouched, so the window keeps working;
//   * destroying the *window* invalidates the surface: `valid()` is false and both `desc()` and
//     `native_handle()` answer zeroed, so a stale renderer cannot read a released pointer.
//
// The second direction is what makes the first meaningful. A type that "survives" destruction could
// still own a handle; a type that goes invalid when its owner disappears demonstrably does not.

#include "tessera/hal/surface.hpp"

#include <cstdint>
#include <iostream>

namespace {

using tessera::hal::NativeSurface;
using tessera::hal::NativeWindowHandle;
using tessera::hal::SurfaceDesc;
using tessera::hal::SurfaceLifetime;
using tessera::hal::WindowSystem;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "native_surface: " << message << "\n";
        ++failures;
    }
    return condition;
}

// The owner side: the object a platform window would be. It owns the liveness record and the handle
// values, and it counts every teardown it performs, so a test can tell "the surface left my window
// alone" from "nothing checked".
class FakeWindow {
public:
    FakeWindow()
        : lifetime(),
          handles{reinterpret_cast<void*>(0x1234), reinterpret_cast<void*>(0x5678)} {}

    [[nodiscard]] SurfaceDesc desc() const {
        SurfaceDesc value;
        value.width_px = 96;
        value.height_px = 64;
        value.dpi_scale = 2.0f;
        value.window_system = WindowSystem::Win32;
        return value;
    }

    // What a platform window's destruction does: release the native handles. The surface must never
    // be able to provoke this.
    void destroy() {
        ++teardowns;
        lifetime.revoke();
        handles = NativeWindowHandle{};
    }

    [[nodiscard]] NativeSurface borrow(std::uint64_t id = 7) const {
        return NativeSurface::borrow(lifetime, desc(), handles, id);
    }

    SurfaceLifetime lifetime;
    NativeWindowHandle handles;
    int teardowns = 0;
};

void test_surface_destruction_leaves_the_window_working() {
    FakeWindow window;
    const NativeWindowHandle before = window.handles;

    {
        // Several borrowed views, including a copy, all destroyed at the end of this scope.
        const NativeSurface surface = window.borrow();
        NativeSurface copy = surface;
        expect(surface.valid() && copy.valid(), "a borrowed surface is valid");
    }

    // The window is untouched: it was never asked to tear down, its liveness is intact, and the
    // handle values it reports are the same pointers, so it keeps working.
    expect(window.teardowns == 0, "destroying a surface never destroys the window");
    expect(window.lifetime.alive(), "the window's liveness survives its surfaces");
    expect(window.handles.window == before.window && window.handles.display == before.display,
           "the surface did not release or clear the window's handles");

    // And it can still hand out a new surface, which is the observable "the window still works".
    const NativeSurface again = window.borrow();
    expect(again.valid(), "the window still lends surfaces after its old ones are gone");
}

void test_window_destruction_invalidates_the_surface() {
    FakeWindow window;
    const NativeSurface surface = window.borrow();
    NativeSurface surviving_copy = surface;
    expect(surface.valid(), "the surface is valid while its window is");

    window.destroy();

    // A surface that owned the handle would still report one here; a borrower reports nothing.
    expect(!surface.valid(), "destroying the window invalidates the surface");
    expect(!surviving_copy.valid(), "every copy of the surface goes invalid together");
    expect(surface.native_handle().window == nullptr && surface.native_handle().display == nullptr,
           "an invalid surface reports zeroed handles, never a released pointer");
    expect(surface.desc().width_px == 0 && surface.desc().height_px == 0,
           "an invalid surface reports no geometry");
    expect(surface.id() == 0, "an invalid surface reports no id");
    expect(window.teardowns == 1, "the window tore down exactly once, on its own behalf");
}

void test_revoke_invalidates_without_destroying_the_owner() {
    FakeWindow window;
    const NativeSurface surface = window.borrow();
    window.lifetime.revoke();

    expect(!surface.valid(), "an explicit revoke invalidates the surface");
    expect(window.teardowns == 0, "revoke is the owner's call, not a window destruction");
    // A second revoke and a later destruction stay harmless: the rule is idempotent.
    window.lifetime.revoke();
    window.destroy();
    expect(!surface.valid(), "the surface stays invalid");
}

void test_borrowed_values_are_frozen() {
    FakeWindow window;
    const NativeSurface surface = window.borrow(11);

    expect(surface.id() == 11, "the surface id is the owner's");
    expect(surface.desc().width_px == 96 && surface.desc().height_px == 64,
           "the description is the one the owner passed");
    expect(surface.desc().dpi_scale == 2.0f, "the dpi scale is preserved");
    expect(surface.desc().window_system == WindowSystem::Win32, "the window family is preserved");
    expect(surface.native_handle().window == window.handles.window,
           "the borrowed window handle is the owner's");
    expect(surface.native_handle().display == window.handles.display,
           "the borrowed display handle is the owner's");

    // A second borrow of the same window is an equal surface: same owner, same id.
    const NativeSurface same = window.borrow(11);
    expect(same.id() == surface.id() && same.valid(), "two borrows of one window agree");
}

void test_default_surface_is_inert() {
    const NativeSurface none;
    expect(!none.valid(), "a default surface is invalid");
    expect(none.id() == 0, "a default surface has no id");
    expect(none.desc().width_px == 0, "a default surface has no geometry");
    expect(none.native_handle().window == nullptr, "a default surface has no handle");
}

void test_offscreen_surface_is_valid_without_handles() {
    // The headless case the renderer's offscreen path depends on: a surface with no native target
    // is still a surface, so a renderer over it composites into a caller-provided target.
    FakeWindow window;
    SurfaceDesc offscreen = window.desc();
    offscreen.window_system = WindowSystem::NoWindow;
    const NativeSurface surface = NativeSurface::borrow(window.lifetime, offscreen);

    expect(surface.valid(), "an offscreen surface is valid");
    expect(surface.desc().window_system == WindowSystem::NoWindow,
           "an offscreen surface reports no window family");
    expect(surface.native_handle().window == nullptr, "an offscreen surface carries no handle");
}

void test_zero_sized_desc_is_not_compositable() {
    FakeWindow window;
    SurfaceDesc empty;
    empty.window_system = WindowSystem::Win32;
    const NativeSurface surface = NativeSurface::borrow(window.lifetime, empty);
    expect(!surface.valid(), "a zero-sized surface is not compositable");
}

} // namespace

int main() {
    test_surface_destruction_leaves_the_window_working();
    test_window_destruction_invalidates_the_surface();
    test_revoke_invalidates_without_destroying_the_owner();
    test_borrowed_values_are_frozen();
    test_default_surface_is_inert();
    test_offscreen_surface_is_valid_without_handles();
    test_zero_sized_desc_is_not_compositable();
    if (failures != 0) {
        std::cerr << "native_surface: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "native_surface: ok\n";
    return 0;
}
