// SPDX-License-Identifier: MIT
//
// The shell instance the Android JNI bridges share (issue #189).
//
// Why this header exists: `jni_bridge.cpp` owns the surface, metrics, touch and IME seams, and
// `a11y_bridge.cpp` owns the accessibility seam. Both need the same `Shell`, and neither may
// include the other's translation unit. The type is one struct of borrowed handles, so it lives
// here rather than being duplicated or reached through a getter.

#pragma once

#include "android_motion_translation.hpp"
#include "scene.hpp"
#include "tessera/app/window.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/accessibility/snapshot_provider.hpp"

#include <android/native_window.h>
#include <atomic>
#include <jni.h>
#include <memory>
#include <thread>

namespace tessera::android_shell {

// One shell instance: the scene, the host, the window it owns, and the accessibility provider the
// platform's UI thread queries. It is created when the Java view is created and destroyed when the
// view is destroyed, so its lifetime is the view's.
struct Shell {
    std::unique_ptr<ShellScene> scene;
    std::unique_ptr<tessera::app::WindowHost> host;
    tessera::hal::Window* window = nullptr;
    ANativeWindow* native_window = nullptr;
    float dpi_scale = 1.0f;
    // The gesture state the Java motion translation needs: the pointer id is fixed at DOWN because
    // Android renumbers indices as fingers lift.
    tessera::hal::android::MotionGesture gesture;
    // Whether the IME has an active composition, so a composing update becomes Start then Update.
    bool composing = false;
    // The frame loop runs on its own thread. `WindowHost` is single-threaded and must be created,
    // run and destroyed on the thread that owns the window, so the render thread owns the host and
    // the Java thread only signals it.
    std::thread render_thread;
    std::atomic<bool> running{false};
    // The pull-side semantics provider the platform's `AccessibilityNodeProvider` reads. It has its
    // own internal mutex, so the platform thread may query it while the render thread publishes.
    // Created with the shell rather than with the surface: a screen reader can ask for the tree
    // before the first frame, and an empty tree is a truthful answer.
    std::unique_ptr<tessera::ui::a11y::SnapshotProvider> a11y =
        std::make_unique<tessera::ui::a11y::SnapshotProvider>();
};

// The shell behind a Java `long` handle, or null for a zero handle.
[[nodiscard]] inline Shell* shell_from(jlong handle) {
    return reinterpret_cast<Shell*>(static_cast<std::intptr_t>(handle));
}

} // namespace tessera::android_shell
