// SPDX-License-Identifier: MIT

#pragma once

namespace tessera::hal {

// Opaque identity of a native presentation surface the CALLER owns, for platforms where the OS
// hands the app a surface instead of letting it create a window (Android's `ANativeWindow*`).
//
// It mirrors `AccessibilityHostHandle`: UI and app code pass it by value and never inspect, cast or
// dereference `token`. Only a platform translation unit may interpret it, so no OS handle type
// leaks into a public header (ADR-020 rule 1). A null token means "the caller has no surface to
// hand over", which is the answer on every platform that creates its own window.
//
// The caller keeps ownership: the platform takes its own reference where the OS supports one
// (`ANativeWindow_acquire`), so the caller may release its own at any point after `create_window`.
struct NativeSurfaceToken {
    void* token = nullptr;

    [[nodiscard]] bool valid() const noexcept {
        return token != nullptr;
    }
};

} // namespace tessera::hal
