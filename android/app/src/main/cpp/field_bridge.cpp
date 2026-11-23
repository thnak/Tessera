// SPDX-License-Identifier: MIT
//
// The Android shell's field and clipboard JNI entry points (issue #189,
// docs/guides/android-lane.md sections 3.13 and 3.16).
//
// Split out of `jni_bridge.cpp` when the clipboard landed: the surface, metrics, fold and touch
// seams are one responsibility, and "what the platform asks the engine about its text field" is
// another. Both include <jni.h>; neither includes the other.

#include "android_caret_translation.hpp"
#include "caret_bridge.hpp"
#include "shell.hpp"

#include <jni.h>

using tessera::android_shell::Shell;
using tessera::android_shell::shell_from;

extern "C" {

// Whether a point is on the text field, in LOGICAL pixels. The view asks this before it shows the
// keyboard: a custom-rendered field cannot focus itself the way an `EditText` does, so without the
// answer the keyboard appears on every tap anywhere in the window.
JNIEXPORT jboolean JNICALL Java_dev_tessera_app_TesseraView_nativeTapOnField(JNIEnv*, jclass,
                                                                             jlong handle, jfloat x,
                                                                             jfloat y) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr) {
        return JNI_FALSE;
    }
    tessera::hal::android::CaretField field;
    if (!tessera::android_shell::caret_field_for_shell(*shell, field)) {
        return JNI_FALSE;
    }
    tessera::hal::android::CaretSample sample;
    sample.x = x;
    sample.y = y;
    sample.dpi_scale = 1.0f;
    return tessera::hal::android::caret_from_point(field, sample).inside ? JNI_TRUE : JNI_FALSE;
}

// The platform's Copy/Cut/Paste. A phone has no Ctrl key, so the IME's context menu and the
// platform's text toolbar call these instead of the engine's key chords.
JNIEXPORT jboolean JNICALL Java_dev_tessera_app_TesseraView_nativeClipboardCopy(JNIEnv*, jclass,
                                                                                jlong handle) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr) {
        return JNI_FALSE;
    }
    return tessera::android_shell::clipboard_copy(*shell) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_dev_tessera_app_TesseraView_nativeClipboardCut(JNIEnv*, jclass,
                                                                               jlong handle) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr) {
        return JNI_FALSE;
    }
    return tessera::android_shell::clipboard_cut(*shell) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL Java_dev_tessera_app_TesseraView_nativeClipboardPaste(JNIEnv*, jclass,
                                                                                 jlong handle) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr) {
        return JNI_FALSE;
    }
    return tessera::android_shell::clipboard_paste(*shell) ? JNI_TRUE : JNI_FALSE;
}

// The platform's "Select all". A phone has no Ctrl+A, so the IME's context menu and the platform's
// text toolbar reach the buffer through this rather than through the key chord.
JNIEXPORT jboolean JNICALL Java_dev_tessera_app_TesseraView_nativeSelectAll(JNIEnv*, jclass,
                                                                            jlong handle) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr) {
        return JNI_FALSE;
    }
    return tessera::android_shell::select_all(*shell) ? JNI_TRUE : JNI_FALSE;
}

// The selection's state, for the platform's floating toolbar. `out` is a float[3]: has-selection,
// anchor x, anchor y (logical pixels). A float array rather than three calls keeps the answer
// consistent: the selection cannot change between the "is there one" and "where" reads.
JNIEXPORT void JNICALL Java_dev_tessera_app_TesseraView_nativeSelectionState(JNIEnv* env, jclass,
                                                                             jlong handle,
                                                                             jfloatArray out) {
    if (out == nullptr || env->GetArrayLength(out) < 3) {
        return;
    }
    Shell* shell = shell_from(handle);
    jfloat values[3] = {0.0f, 0.0f, 0.0f};
    if (shell != nullptr) {
        const tessera::android_shell::SelectionState state =
            tessera::android_shell::selection_state(*shell);
        values[0] = state.has_selection ? 1.0f : 0.0f;
        values[1] = state.anchor_x;
        values[2] = state.anchor_y;
    }
    env->SetFloatArrayRegion(out, 0, 3, values);
}

} // extern "C"
