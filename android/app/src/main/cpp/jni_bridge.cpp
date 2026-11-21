// SPDX-License-Identifier: MIT
//
// The JNI bridge between the Java shell and the engine (issue #189).
//
// This is the only translation unit that includes <jni.h>. It does three things and nothing else:
//
//   1. Turns a Java `Surface` into the `ANativeWindow*` the HAL window adopts, and creates the
//      `WindowHost` around it.
//   2. Forwards the platform's insets/cutouts to the window backend's setters, which decode them
//      with the pure rules in android_metrics_translation.cpp.
//   3. Forwards a Java `MotionEvent` to the window's input translator.
//
// The engine never sees a `jobject`, and this file never interprets a scene: it is a thin,
// value-only bridge, which is what docs/design/bridges.md section 3 asks for.

#include "scene.hpp"
#include "tessera/app/window.hpp"
#include "tessera/hal/window.hpp"

#include <android/log.h>
#include <android/native_window_jni.h>
#include <atomic>
#include <cstdint>
#include <jni.h>
#include <memory>
#include <thread>
#include <vector>

namespace {

constexpr const char* kLogTag = "tessera";

void log_info(const char* message) {
    __android_log_write(ANDROID_LOG_INFO, kLogTag, message);
}

// One shell instance: the scene, the host and the window it owns. It is created when the Java view
// is created and destroyed when the view is destroyed, so its lifetime is the view's.
struct Shell {
    std::unique_ptr<tessera::android_shell::ShellScene> scene;
    std::unique_ptr<tessera::app::WindowHost> host;
    tessera::hal::Window* window = nullptr;
    ANativeWindow* native_window = nullptr;
    float dpi_scale = 1.0f;
    // The frame loop runs on its own thread. `WindowHost` is single-threaded and must be created,
    // run and destroyed on the thread that owns the window, so the render thread owns the host and
    // the Java thread only signals it.
    std::thread render_thread;
    std::atomic<bool> running{false};
};

Shell* shell_from(jlong handle) {
    return reinterpret_cast<Shell*>(static_cast<std::intptr_t>(handle));
}

// The display density, read from the Java view's resources. 1.0 = mdpi.
float display_scale(JNIEnv* env, jobject view) {
    jclass view_class = env->GetObjectClass(view);
    if (view_class == nullptr) {
        return 1.0f;
    }
    jmethodID get_resources =
        env->GetMethodID(view_class, "getResources", "()Landroid/content/res/Resources;");
    if (get_resources == nullptr) {
        env->DeleteLocalRef(view_class);
        return 1.0f;
    }
    jobject resources = env->CallObjectMethod(view, get_resources);
    if (resources == nullptr) {
        env->DeleteLocalRef(view_class);
        return 1.0f;
    }
    jclass resources_class = env->GetObjectClass(resources);
    jmethodID get_display_metrics =
        env->GetMethodID(resources_class, "getDisplayMetrics", "()Landroid/util/DisplayMetrics;");
    if (get_display_metrics == nullptr) {
        env->DeleteLocalRef(resources_class);
        env->DeleteLocalRef(resources);
        env->DeleteLocalRef(view_class);
        return 1.0f;
    }
    jobject metrics = env->CallObjectMethod(resources, get_display_metrics);
    if (metrics == nullptr) {
        env->DeleteLocalRef(resources_class);
        env->DeleteLocalRef(resources);
        env->DeleteLocalRef(view_class);
        return 1.0f;
    }
    jclass metrics_class = env->GetObjectClass(metrics);
    jfieldID density_field = env->GetFieldID(metrics_class, "density", "F");
    float density = 1.0f;
    if (density_field != nullptr) {
        density = env->GetFloatField(metrics, density_field);
    }
    env->DeleteLocalRef(metrics_class);
    env->DeleteLocalRef(metrics);
    env->DeleteLocalRef(resources_class);
    env->DeleteLocalRef(resources);
    env->DeleteLocalRef(view_class);
    return density > 0.0f ? density : 1.0f;
}

// Creates the host around the surface and starts the frame loop. Called from `surfaceCreated`,
// which is the first moment the engine may render.
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

    if (shell.scene == nullptr) {
        shell.scene = std::make_unique<tessera::android_shell::ShellScene>();
    }
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
    shell.running.store(true);
    shell.render_thread = std::thread([&shell] {
        log_info("render thread entered");
        (void)shell.host->run(0);
        log_info("render thread exited");
        shell.running.store(false);
    });
}

// Stops the frame loop and destroys the host. Called from `surfaceDestroyed` and `nativeDestroy`,
// so the host is gone before the surface can be reused.
void stop_host(Shell& shell) {
    if (shell.host == nullptr) {
        return;
    }
    // `run(0)` returns when the window closes, so closing it is how the loop is asked to stop.
    if (shell.window != nullptr) {
        shell.window->close();
    }
    if (shell.render_thread.joinable()) {
        shell.render_thread.join();
    }
    shell.host.reset();
    shell.window = nullptr;
    log_info("window host stopped");
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_dev_tessera_app_TesseraView_nativeCreate(JNIEnv* env, jclass,
                                                                      jobject view,
                                                                      jobject surface) {
    auto shell = std::make_unique<Shell>();
    shell->dpi_scale = display_scale(env, view);
    if (surface != nullptr) {
        shell->native_window = ANativeWindow_fromSurface(env, surface);
    }
    log_info("native shell created");
    return static_cast<jlong>(reinterpret_cast<std::intptr_t>(shell.release()));
}

JNIEXPORT void JNICALL Java_dev_tessera_app_TesseraView_nativeDestroy(JNIEnv*, jclass,
                                                                      jlong handle) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr) {
        return;
    }
    stop_host(*shell);

    if (shell->native_window != nullptr) {
        ANativeWindow_release(shell->native_window);
        shell->native_window = nullptr;
    }
    delete shell;
    log_info("native shell destroyed");
}

JNIEXPORT void JNICALL Java_dev_tessera_app_TesseraView_nativeSurfaceCreated(JNIEnv* env, jclass,
                                                                             jlong handle,
                                                                             jobject surface) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr || surface == nullptr) {
        return;
    }
    if (shell->native_window != nullptr) {
        ANativeWindow_release(shell->native_window);
    }
    shell->native_window = ANativeWindow_fromSurface(env, surface);
    log_info("surface created");
    start_host(*shell);
}

JNIEXPORT void JNICALL Java_dev_tessera_app_TesseraView_nativeSurfaceChanged(JNIEnv*, jclass,
                                                                             jlong handle, jobject,
                                                                             jint width,
                                                                             jint height) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr || shell->window == nullptr) {
        return;
    }
    // The window backend recomputes its metrics from the ANativeWindow and queues a coalesced
    // Resize; the width/height the platform reports are already what the surface carries.
    (void)width;
    (void)height;
}

JNIEXPORT void JNICALL Java_dev_tessera_app_TesseraView_nativeSurfaceDestroyed(JNIEnv*, jclass,
                                                                               jlong handle) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr) {
        return;
    }
    // Stop rendering before the surface goes away: the host holds a borrowed surface, and a present
    // into a destroyed one is undefined behaviour. The scene survives, so the app keeps its state.
    stop_host(*shell);

    log_info("surface destroyed");
}

JNIEXPORT void JNICALL Java_dev_tessera_app_TesseraView_nativeSetInsets(
    JNIEnv*, jclass, jlong handle, jint bars_left, jint bars_top, jint bars_right, jint bars_bottom,
    jint ime_left, jint ime_top, jint ime_right, jint ime_bottom, jint gesture_left,
    jint gesture_top, jint gesture_right, jint gesture_bottom) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr || shell->window == nullptr) {
        return;
    }
    // The decode rules live in C++ and are unit-tested; this only forwards the platform values.
    tessera::hal::Window::PhysicalInsets bars{bars_left, bars_top, bars_right, bars_bottom};
    tessera::hal::Window::PhysicalInsets ime{ime_left, ime_top, ime_right, ime_bottom};
    tessera::hal::Window::PhysicalInsets gestures{gesture_left, gesture_top, gesture_right,
                                                  gesture_bottom};
    shell->window->set_physical_insets(bars, ime, gestures);
}

JNIEXPORT void JNICALL Java_dev_tessera_app_TesseraView_nativeSetCutouts(JNIEnv* env, jclass,
                                                                         jlong handle,
                                                                         jintArray rects) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr || shell->window == nullptr || rects == nullptr) {
        return;
    }
    const jsize length = env->GetArrayLength(rects);
    if (length < 4) {
        return;
    }
    std::vector<jint> flat(static_cast<std::size_t>(length));
    env->GetIntArrayRegion(rects, 0, length, flat.data());
    std::vector<tessera::hal::Window::PhysicalRect> converted;
    converted.reserve(static_cast<std::size_t>(length) / 4);
    for (std::size_t i = 0; i + 3 < flat.size(); i += 4) {
        tessera::hal::Window::PhysicalRect rect;
        rect.left = flat[i];
        rect.top = flat[i + 1];
        rect.right = flat[i + 2];
        rect.bottom = flat[i + 3];
        converted.push_back(rect);
    }
    shell->window->set_physical_cutouts(converted);
}

JNIEXPORT jboolean JNICALL Java_dev_tessera_app_TesseraView_nativeOnTouch(JNIEnv*, jclass,
                                                                          jlong handle,
                                                                          jobject event) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr || shell->window == nullptr) {
        return JNI_FALSE;
    }
    // A Java MotionEvent is not an AInputEvent: the NDK's AInputEvent comes from the input queue,
    // not from a Java object. Translating a Java event needs its own reader, which is the next
    // slice; today the native input path is the AInputQueue one.
    (void)event;
    return JNI_FALSE;
}

} // extern "C"
