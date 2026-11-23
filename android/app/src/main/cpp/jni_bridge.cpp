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

#include "android_caret_translation.hpp"
#include "android_input_translation.hpp"
#include "android_motion_translation.hpp"
#include "caret_bridge.hpp"
#include "scene.hpp"
#include "shell.hpp"
#include "tessera/app/window.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/runtime/steady_clock.hpp"
#include "tessera/ui/text/text_measure.hpp"

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

using tessera::android_shell::Shell;
using tessera::android_shell::shell_from;

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

// Reads the scalar fields of one Java `MotionEvent` into the pure `MotionSample` the mapping rules
// take. This is the only place that touches a Java object's getters: the mapping itself is pure and
// unit-tested on every lane (`android_motion_translation.cpp`).
//
// Every getter is looked up once per call. That is more lookups than caching them in a struct, but
// a touch stream is tens of events per second, not thousands, and caching would need a lifetime for
// the cached IDs that this bridge does not have.
bool read_motion_event(JNIEnv* env, jobject event, tessera::hal::android::MotionSample& out) {
    if (event == nullptr) {
        return false;
    }
    jclass cls = env->GetObjectClass(event);
    if (cls == nullptr) {
        return false;
    }

    // getAction() packs the pointer index into the high byte, which is exactly the shape the pure
    // rules expect, so no repacking happens here.
    auto call_int = [&](const char* name) -> std::int32_t {
        jmethodID method = env->GetMethodID(cls, name, "()I");
        if (method == nullptr) {
            return 0;
        }
        return env->CallIntMethod(event, method);
    };
    auto call_int_index = [&](const char* name, jint index) -> std::int32_t {
        jmethodID method = env->GetMethodID(cls, name, "(I)I");
        if (method == nullptr) {
            return 0;
        }
        return env->CallIntMethod(event, method, index);
    };
    auto call_float_index = [&](const char* name, jint index) -> jfloat {
        jmethodID method = env->GetMethodID(cls, name, "(I)F");
        if (method == nullptr) {
            return 0.0F;
        }
        return env->CallFloatMethod(event, method, index);
    };

    out.action = call_int("getAction");

    // The pointer the action names: the high byte for a pointer action, index 0 otherwise. The pure
    // rules compute the same index, but the getters need it now, so it is derived here too.
    const std::int32_t action_bits = out.action & tessera::hal::android::kActionMask;
    const bool names_pointer = action_bits == tessera::hal::android::kActionPointerDown ||
                               action_bits == tessera::hal::android::kActionPointerUp;
    const jint index =
        names_pointer ? (out.action >> tessera::hal::android::kActionPointerIndexShift) : 0;

    out.device_id = call_int("getDeviceId");
    out.source = call_int("getSource");
    out.pointer_id = call_int_index("getPointerId", index);
    out.tool_type = call_int_index("getToolType", index);
    out.x = static_cast<float>(call_float_index("getX", index));
    out.y = static_cast<float>(call_float_index("getY", index));
    out.pressure = static_cast<float>(call_float_index("getPressure", index));

    jmethodID get_event_time = env->GetMethodID(cls, "getEventTime", "()J");
    if (get_event_time != nullptr) {
        out.event_time_nanos = env->CallLongMethod(event, get_event_time);
    }

    // The two scroll axes only exist on an ACTION_SCROLL, and asking for an axis on any other event
    // is wasted work, so they are read only then.
    if (action_bits == tessera::hal::android::kActionScroll) {
        jmethodID get_axis = env->GetMethodID(cls, "getAxisValue", "(I)F");
        if (get_axis != nullptr) {
            // MotionEvent.AXIS_VSCROLL = 9, AXIS_HSCROLL = 10.
            out.vscroll = static_cast<float>(env->CallFloatMethod(event, get_axis, 9));
            out.hscroll = static_cast<float>(env->CallFloatMethod(event, get_axis, 10));
        }
    }

    env->DeleteLocalRef(cls);
    return true;
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
    // The surface extent is the one thing this callback exists to report. It used to be discarded
    // on the assumption that the backend would re-read the ANativeWindow itself, but nothing calls
    // it: a fold that switches to the cover display changes the extent from 2076x2152 to 1080x2364
    // and the engine kept rendering at the old size, which is a black screen on the new display.
    //
    // `set_surface_extent` is the HAL seam for exactly this: it recomputes the metrics and queues a
    // coalesced Resize, so the driver resizes the swapchain and re-lays out.
    if (width > 0 && height > 0) {
        shell->window->set_surface_extent(static_cast<std::uint32_t>(width),
                                          static_cast<std::uint32_t>(height));
    }
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
    tessera::hal::PhysicalInsets bars{bars_left, bars_top, bars_right, bars_bottom};
    tessera::hal::PhysicalInsets ime{ime_left, ime_top, ime_right, ime_bottom};
    tessera::hal::PhysicalInsets gestures{gesture_left, gesture_top, gesture_right, gesture_bottom};
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
    std::vector<tessera::hal::PhysicalRect> converted;
    converted.reserve(static_cast<std::size_t>(length) / 4);
    for (std::size_t i = 0; i + 3 < flat.size(); i += 4) {
        tessera::hal::PhysicalRect rect;
        rect.left = flat[i];
        rect.top = flat[i + 1];
        rect.right = flat[i + 2];
        rect.bottom = flat[i + 3];
        converted.push_back(rect);
    }
    shell->window->set_physical_cutouts(converted);
}

JNIEXPORT void JNICALL Java_dev_tessera_app_TesseraView_nativeSetFold(JNIEnv*, jclass, jlong handle,
                                                                      jint posture, jint axis,
                                                                      jint left, jint top,
                                                                      jint right, jint bottom) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr || shell->window == nullptr) {
        return;
    }
    // The Java reader sends the engine's own enum values, so this is a cast rather than a mapping:
    // the mapping from the library's objects to those values lives in TesseraFoldReader, where it
    // is one place instead of two. The hinge is physical pixels, which is what the decode rules
    // take.
    tessera::hal::PhysicalFold fold;
    fold.posture = static_cast<tessera::hal::PhysicalFoldPosture>(posture);
    fold.axis = static_cast<tessera::hal::PhysicalFoldAxis>(axis);
    fold.hinge.left = left;
    fold.hinge.top = top;
    fold.hinge.right = right;
    fold.hinge.bottom = bottom;
    shell->window->set_physical_fold(fold);
}

JNIEXPORT void JNICALL Java_dev_tessera_app_TesseraView_nativeSetHingeAngle(JNIEnv*, jclass,
                                                                            jlong handle,
                                                                            jfloat degrees) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr || shell->window == nullptr) {
        return;
    }
    // The sensor reports degrees directly, so there is nothing to convert here: the backend clamps
    // and validates the value (a vendor driver can report out of range or a NaN), and a negative
    // reading is the "no sensor" sentinel the Java reader sends when the sensor goes away.
    shell->window->set_physical_hinge_angle(static_cast<float>(degrees));
}

JNIEXPORT jboolean JNICALL Java_dev_tessera_app_TesseraView_nativeOnTouch(JNIEnv* env, jclass,
                                                                          jlong handle,
                                                                          jobject event) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr || shell->window == nullptr) {
        return JNI_FALSE;
    }
    // The shell is a Java activity, so touch arrives as a MotionEvent rather than as the NDK's
    // AInputEvent. The bridge reads the getters; the mapping is the pure, unit-tested translation
    // in android_motion_translation.cpp.
    tessera::hal::android::MotionSample sample;
    if (!read_motion_event(env, event, sample)) {
        return JNI_FALSE;
    }
    const tessera::hal::android::MotionTranslation out =
        tessera::hal::android::translate_motion_sample(sample, shell->dpi_scale, shell->gesture);
    if (!out.translated) {
        // Returning false lets the platform fall back on an event the engine cannot use, instead of
        // swallowing it.
        return JNI_FALSE;
    }
    // Returns true only when the event actually reached the queue, so the platform keeps sending
    // the rest of the gesture.
    const bool posted = shell->window->post_input_event(out.event);
    // What the press MEANS is the shell's decision, not the queue's: a tap places the caret, a held
    // press starts a selection and a drag extends it. The state machine is pure and unit-tested
    // (`android_selection`), so this only maps the platform's action onto its three inputs and
    // applies the answer.
    const bool down = out.event.type == tessera::ui::InputEventType::PointerDown;
    const bool up = out.event.type == tessera::ui::InputEventType::PointerUp ||
                    out.event.type == tessera::ui::InputEventType::PointerCancel;
    const bool move = out.event.type == tessera::ui::InputEventType::PointerMove;
    if (posted && (down || up || move)) {
        // The slop is 8 dp, so it scales with the density; the event is already in logical pixels.
        tessera::hal::android::SelectionGestureConfig config;
        config.touch_slop = 8.0f;
        // `selecting()` is read BEFORE the feed, because an up clears it: the answer to "was this
        // gesture a selection?" is the state the gesture was in, not the state it left.
        const bool was_selecting = shell->selection_gesture.selecting();
        if (down) {
            // Remembered for the long-press tick: a finger that holds still sends no samples, so
            // the tick has no event to read the anchor from.
            shell->selection_press_x = out.event.x;
            shell->selection_press_y = out.event.y;
            // A static scene idles (ADR-019), so nothing would tick the long-press timeout. Asking
            // for a frame is what wakes the loop for the duration of the press; the caret tick then
            // runs and can turn the press into a selection.
            shell->window->request_redraw();
        }
        const tessera::hal::android::SelectionGestureAction action =
            shell->selection_gesture.feed(down, up, out.event.x, out.event.y,
                                          tessera::runtime::SteadyClock{}.now_seconds(), config);
        switch (action) {
        case tessera::hal::android::SelectionGestureAction::BeginSelection:
            // The long-press anchor is the caret the press started on, so the drag grows from
            // there. Placing it first is what makes `extend_selection_to` keep that anchor.
            tessera::android_shell::place_caret_from_touch(*shell, out.event.x, out.event.y);
            break;
        case tessera::hal::android::SelectionGestureAction::ExtendSelection:
            tessera::android_shell::extend_selection_from_touch(*shell, out.event.x, out.event.y);
            break;
        case tessera::hal::android::SelectionGestureAction::EndSelection:
        case tessera::hal::android::SelectionGestureAction::None:
            break;
        }
        // A tap - a press that never became a selection - places the caret on the way up, which is
        // what makes a tap feel immediate rather than waiting for the long-press timeout.
        if (up && !was_selecting) {
            tessera::android_shell::place_caret_from_touch(*shell, out.event.x, out.event.y);
        }
    }
    return posted ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
