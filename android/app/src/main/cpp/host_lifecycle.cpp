// SPDX-License-Identifier: MIT
//
// The Android shell's frame-loop lifecycle (issue #189, docs/guides/android-lane.md section 3.19).
//
// Split out of `jni_bridge.cpp` when the pause/resume seam landed: the JNI surface, the metrics and
// the input translation are one responsibility, and "start and stop the render thread" is another.
//
// The reason this is its own file rather than two functions at the bottom of the bridge is the
// bounded join: a render thread that is inside a present cannot be stopped from the UI thread
// without deadlocking it, and that rule needs the room to be explained where it lives.

#include "caret_bridge.hpp"
#include "scene.hpp"
#include "shell.hpp"
#include "tessera/app/window.hpp"
#include "tessera/hal/window.hpp"

#include <android/log.h>
#include <chrono>
#include <memory>
#include <thread>

namespace tessera::android_shell {
namespace {

constexpr const char* kLogTag = "tessera";

void log_info(const char* message) {
    __android_log_write(ANDROID_LOG_INFO, kLogTag, message);
}

} // namespace

// The lifecycle seam, and the frame loop. These two are the shell's only way to start and stop
// rendering, and `start_host` is idempotent so a redundant `surfaceCreated` costs nothing.
//
// They live outside the anonymous namespace because `field_bridge.cpp` (which owns the pause/resume
// JNI entry points) needs them: the app going to the background does NOT destroy the surface on
// current Android, so `pauseRendering` has to stop the host itself rather than rely on
// `surfaceDestroyed`.
void start_host(Shell& shell) {
    if (shell.host != nullptr || shell.native_window == nullptr) {
        return;
    }
    tessera::app::WindowOptions options;
    options.title = "Tessera";
    options.visible = true;
    options.vsync = true;
    // The opaque token seam: the backend interprets this as an ANativeWindow*.
    options.surface.token = shell.native_window;
    options.dpi_scale = shell.dpi_scale;
    // The text stack requires a font: TextStack throws on an empty path, which aborts the process.
    // Android ships Roboto at a stable path, so the shell uses it rather than bundling a copy.
    options.font_path = "/system/fonts/Roboto-Regular.ttf";
    // The same face, loaded once for the caret hit test: a tap must resolve to the caret under the
    // finger, so the hit test has to measure the glyphs paint draws. A face that fails to load
    // leaves the metrics provider null, and `caret_field_for_shell` then declines - a tap that
    // cannot place the caret leaves it alone instead of jumping it to the start.
    if (!shell.field_face) {
        auto loaded = tessera::ui::text::FontFace::load_from_file(options.font_path);
        if (loaded.has_value()) {
            shell.field_face = std::move(*loaded);
            shell.field_metrics =
                std::make_unique<tessera::ui::text::FreeTypeFontMetricsProvider>(shell.field_face);
            // The advance source paint measures with; the hit test must use the same one.
            shell.field_advance =
                std::make_unique<tessera::ui::paint::FontPaintTextMetrics>(shell.field_face, 1);
            // The field's authored style: `TextInput { font-size: 16px }` in the shell's sheet.
            shell.field_text_style.font_size = 16.0f;
            shell.field_font_metrics = shell.field_metrics->metrics(shell.field_text_style);
        }
    }
    // The pull-side accessibility provider. Android's `AccessibilityNodeProvider` asks for one node
    // at a time, so the driver keeps this provider current and the Java provider queries it from
    // the platform's thread. The shell owns it, so it outlives the host.
    options.accessibility_provider = shell.a11y.get();

    if (shell.scene == nullptr) {
        shell.scene = std::make_unique<tessera::android_shell::ShellScene>();
    }
    // The edit runtime publishes the caret and the committed text into the field's paint data, and
    // turns each committed edit into the scene's `TextChange`. Attaching before the host exists is
    // safe: the node is already in the tree, and the runtime only reads it on an event.
    shell.editor_runtime.attach(shell.scene->input_label);
    // The clipboard the editor's Copy/Cut/Paste chords write through. The Android backend keeps a
    // cache the UI thread refreshes, so the engine's synchronous seam never blocks on the UI
    // thread.
    shell.editor.set_clipboard(shell.clipboard.get());
    // The demo field is multi-line, so Enter inserts a newline instead of being dropped. A
    // single-line field ignores Enter by design (submit is a form concern the engine defers), which
    // on a phone reads as "the Enter key is broken" - the shell has no form to submit, so a
    // multi-line field is the honest demo.
    shell.editor.set_multiline(true);
    // The caret blink. The driver owns the clock, but only the shell knows whether the field is
    // focused, so the policy is a callback and the runtime is where the phase lands: without both
    // the caret would be solid, because nothing would ever flip the decoration's phase.
    options.caret_active = &tessera::android_shell::caret_active_for_shell;
    options.caret_active_user = &shell;
    options.caret_target = &shell.editor_runtime;
    shell.host = std::make_unique<tessera::app::WindowHost>(
        shell.scene->root, tessera::android_shell::shell_stylesheet(), options,
        &shell.scene->scheduler);
    shell.window = shell.host->window();
    if (!shell.host->available()) {
        log_info("the window host is unavailable; no frame will be presented");
        return;
    }
    log_info("window host started");

    // The frame loop. `run(0)` blocks until the window closes, which is what a render thread wants:
    // it owns the host and the pacer, and the Java thread only signals it to stop. `pump_once`
    // would need this thread to spin, and a spin on a phone burns battery for nothing.
    //
    // The host pointer is captured BY VALUE. `stop_host` can detach this thread when it is stuck
    // inside a present, and `start_host` then assigns a new host to `shell.host`: a lambda that
    // read `shell.host` would call into the new host from the old thread.
    tessera::app::WindowHost* const host = shell.host.get();
    shell.running.store(true);
    shell.render_thread = std::thread([&shell, host] {
        log_info("render thread entered");
        (void)host->run(0);
        log_info("render thread exited");
        shell.running.store(false);
    });
}

// Stops the frame loop and destroys the host. Called from `surfaceDestroyed`, `onPause` and
// `nativeDestroy`.
//
// The join is BOUNDED. `run(0)` returns when the window closes, but a render thread already inside
// a present cannot abandon it: SurfaceFlinger stops consuming when the app goes to the background,
// so the driver blocks until the buffer queue is released, which does not happen while this thread
// waits. Waiting forever there is a deadlock on the UI thread and an ANR for every touch.
//
// So: ask the loop to stop, give it a moment, and if it is still inside a present, detach it. The
// thread owns nothing that outlives the process - it holds `shell` by reference and the host by
// pointer, and the host is only destroyed after the thread is done or detached, which is why the
// host is moved out here rather than reset in place.
void stop_host(Shell& shell) {
    if (shell.host == nullptr) {
        return;
    }
    // `run(0)` returns when the window closes, so closing it is how the loop is asked to stop. The
    // close also wakes a parked loop (window_android.cpp), which is what makes the common case
    // immediate.
    if (shell.window != nullptr) {
        shell.window->close();
    }
    if (shell.render_thread.joinable()) {
        // Wait for the loop to leave `run(0)`. A thread that is parked wakes on the close; one that
        // is inside a present does not, and is left detached rather than deadlocking the caller.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (shell.running.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (shell.running.load()) {
            log_info("render thread is inside a present; detaching instead of blocking");
            shell.render_thread.detach();
            // The host is leaked on purpose: the detached thread is still using it, and the process
            // is on its way out of the foreground. The scene survives; only the swapchain is lost.
            (void)shell.host.release();
        } else {
            shell.render_thread.join();
            shell.host.reset();
        }
    } else {
        shell.host.reset();
    }
    shell.window = nullptr;
    log_info("window host stopped");
}

} // namespace tessera::android_shell
