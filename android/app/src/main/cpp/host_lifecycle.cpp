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

// Pauses the frame loop WITHOUT tearing it down.
//
// This is the whole fix for going to the background, and the teardown path below is why it took two
// attempts. Stopping the host means closing the window and joining the render thread, and neither
// is safe from the UI thread once the app is leaving: the loop may be parked on a pace deadline (so
// it only sees the close at its next wake) or inside a present (so the driver blocks until the
// buffer queue is released, which needs the compositor this app no longer has). Joining deadlocked
// the UI thread; detaching left a live renderer holding its own swapchain, so the next cycle had
// two renderers competing for one buffer queue and the app came back black.
//
// The engine already models this: `Window::hide()` sets `WindowMetrics::minimized`, and a paused
// window makes the loop idle with zero presents (`window.hpp`, the occlusion/minimize note).
// Nothing has to stop, so nothing has to be joined.
void pause_host(Shell& shell) {
    if (shell.window == nullptr) {
        return;
    }
    shell.window->hide();
    log_info("window hidden");
}

// Destroys the host. Only for teardown (`surfaceDestroyed`, `nativeDestroy`), where the surface is
// already gone and the caller can afford to wait for the render thread.
void stop_host(Shell& shell) {
    if (shell.host == nullptr) {
        return;
    }
    if (shell.window != nullptr) {
        shell.window->close();
    }
    if (shell.render_thread.joinable()) {
        // Generous: a thread parked on a pace deadline only notices the close at its next wake, and
        // one inside a present has to drain first - measured at ~1.5 s on the emulator.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (shell.running.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (!shell.running.load()) {
            shell.render_thread.join();
            shell.host.reset();
        } else {
            log_info("render thread did not stop; detaching (leaked host)");
            shell.render_thread.detach();
            (void)shell.host.release();
        }
    } else {
        shell.host.reset();
    }
    shell.window = nullptr;
    log_info("window host stopped");
}

// Resumes a paused host. The window and the thread were never torn down, so this is a show plus a
// full-repaint latch.
//
// `show()` alone is NOT enough, and that is this function's whole reason for existing. The host
// pauses on the `Minimized`/`Restored` events and resumes on the latter, but a resume edge only
// clears the pause: `FramePipeline::tick` clears the damage accumulator every tick and derives
// `should_render` from the damage plan, so a scene with no dirty node produces an empty scissor
// list and the loop parks without painting. On a fresh window the first layout's dirty flags hide
// that (every node is dirty once), which is why the bug only showed after a surface was recreated:
// the host was rebuilt around an already-settled tree, so nothing was dirty and nothing painted -
// a black window that only a tap (which dirties a node) could fix.
//
// `request_full_repaint()` is the documented lever for exactly this: it latches a full-target plan
// and marks the loop dirty. `WindowHost::set_visible(true)` already calls it for the same reason on
// desktop (src/app/window.cpp), so this is the Android path catching up with that rule rather than
// a new one.
void resume_host(Shell& shell) {
    if (shell.window == nullptr || shell.host == nullptr) {
        return;
    }
    shell.window->show();
    shell.host->host().request_full_repaint();
    shell.window->request_redraw();
    log_info("window shown");
}

} // namespace tessera::android_shell
