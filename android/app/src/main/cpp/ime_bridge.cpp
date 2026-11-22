// SPDX-License-Identifier: MIT
//
// The Android IME bridge (issue #189, docs/design/mobile-integration.md section 3.3).
//
// Split out of `jni_bridge.cpp` when the text queries landed: the surface, metrics, fold and touch
// seams are one responsibility, and the edit controller's callbacks plus the platform's text
// queries are another. Both include <jni.h>; neither includes the other.
//
// The engine owns the buffer, so the platform's `InputConnection` must not keep a copy. Every
// callback here forwards into `Shell::editor` (the `EditController`) and every query reads it back,
// which is what makes the IME's idea of the text and the engine's the same text.

#include "android_ime_translation.hpp"
#include "android_input_translation.hpp"
#include "shell.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using tessera::android_shell::Shell;
using tessera::android_shell::shell_from;

namespace {

// Forwards one normalized input event to the edit controller and requests the frame that shows the
// result. Shared by the IME callbacks and the key path: both produce `ui::InputEvent` and both must
// reach the same controller, so the mapping and the redraw request live in one place.
//
// The event goes straight to the controller, not through the window's input queue. The controller
// is the buffer's owner, so routing through the queue would make the text arrive one frame later
// than the caret the IME is tracking, and the platform would then ask for text around a caret the
// engine has not applied yet.
bool dispatch_to_editor(Shell& shell, const tessera::ui::InputEvent& input) {
    tessera::ui::Event event;
    // The two enums are NOT the same type and do NOT share values: `ui::InputEventType` is the
    // normalized input vocabulary, `ui::EventType` is the scene's. A cast would silently turn a
    // commit into a composition start (EditText is 13 in one and 14 in the other), so the mapping
    // is explicit and total.
    switch (input.type) {
    case tessera::ui::InputEventType::Text:
        event.type = tessera::ui::EventType::Text;
        break;
    case tessera::ui::InputEventType::EditText:
        event.type = tessera::ui::EventType::EditText;
        break;
    case tessera::ui::InputEventType::CompositionStart:
        event.type = tessera::ui::EventType::CompositionStart;
        break;
    case tessera::ui::InputEventType::CompositionUpdate:
        event.type = tessera::ui::EventType::CompositionUpdate;
        break;
    case tessera::ui::InputEventType::CompositionEnd:
        event.type = tessera::ui::EventType::CompositionEnd;
        break;
    case tessera::ui::InputEventType::KeyDown:
        event.type = tessera::ui::EventType::KeyDown;
        break;
    case tessera::ui::InputEventType::KeyUp:
        event.type = tessera::ui::EventType::KeyUp;
        break;
    default:
        // The producers above only emit these seven types; anything else is a bug in the
        // translation, and dropping it is safer than dispatching a wrong event.
        return false;
    }
    event.set_text(std::string_view(input.text.data(), input.text_size));
    event.selection_start = input.selection_start;
    event.selection_end = input.selection_end;
    event.key = input.key;
    event.modifiers = input.modifiers;
    event.repeat = input.repeat;
    event.text_codepoint = input.text_codepoint;
    const bool consumed = shell.editor_runtime.handle_event(event);
    if (consumed && shell.window != nullptr) {
        // The node's paint data changed, so the frame that shows the new text has to be requested.
        shell.window->request_redraw();
    }
    return consumed;
}

} // namespace

extern "C" {
JNIEXPORT jboolean JNICALL Java_dev_tessera_app_TesseraView_nativeIme(
    JNIEnv* env, jclass, jlong handle, jint action, jstring text, jint cursor_utf16,
    jint delete_before, jint delete_after) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr || shell->window == nullptr) {
        return JNI_FALSE;
    }
    // The IME callbacks arrive as Java strings; the bridge reads them into UTF-16 once and the
    // mapping in android_ime_translation.cpp does the conversion to the engine's UTF-8 payload.
    tessera::hal::android::ImeSample sample;
    sample.action = static_cast<tessera::hal::android::ImeAction>(action);
    sample.cursor_utf16 = cursor_utf16;
    sample.delete_before = delete_before;
    sample.delete_after = delete_after;
    if (text != nullptr) {
        const jsize length = env->GetStringLength(text);
        const jsize capped = std::min<jsize>(
            length, static_cast<jsize>(tessera::hal::android::ImeSample::kMaxUtf16));
        if (capped > 0) {
            // `jchar` and `char16_t` are both 16-bit code units but distinct types, so the region
            // is read into a jchar buffer and copied across. A reinterpret_cast would work and is
            // what most bridges do; the copy is a few hundred bytes on a callback that happens once
            // per keystroke, so it is not worth the aliasing risk.
            std::vector<jchar> units(static_cast<std::size_t>(capped));
            env->GetStringRegion(text, 0, capped, units.data());
            for (jsize i = 0; i < capped; ++i) {
                sample.text[i] = static_cast<char16_t>(units[static_cast<std::size_t>(i)]);
            }
            sample.text_length = static_cast<std::size_t>(capped);
        }
    }
    const tessera::hal::android::ImeTranslation out =
        tessera::hal::android::translate_ime_sample(sample, shell->composing);
    if (!out.translated) {
        return JNI_FALSE;
    }
    return dispatch_to_editor(*shell, out.event) ? JNI_TRUE : JNI_FALSE;
}

// The text around the caret, for the platform's `InputConnection` queries. Android asks these to
// build suggestions, to decide whether Backspace has anything to delete, and to place its own
// composing region; a bridge that answers "" makes the IME behave as if the field were always
// empty, which is what a custom-rendered field looks like when the engine owns the buffer.
//
// The answer is a Java `String`, built from the engine's UTF-8 buffer. `before`/`after` are UTF-16
// code-unit counts, which is what the platform asks in; the conversion walks the buffer once.
JNIEXPORT jstring JNICALL Java_dev_tessera_app_TesseraView_nativeTextAround(JNIEnv* env, jclass,
                                                                            jlong handle,
                                                                            jint before,
                                                                            jint after) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr) {
        return env->NewStringUTF("");
    }
    const std::string_view text = shell->editor.buffer().text();
    const std::size_t caret = shell->editor.buffer().caret();
    // The engine's caret is a UTF-8 byte offset; the platform's window is in UTF-16 units. Walking
    // back from the caret by code points is what keeps a surrogate pair from being split.
    std::size_t begin = caret;
    for (jint i = 0; i < before && begin > 0; ++i) {
        --begin;
        while (begin > 0 && (static_cast<unsigned char>(text[begin]) & 0xC0U) == 0x80U) {
            --begin;
        }
    }
    std::size_t end = caret;
    for (jint i = 0; i < after && end < text.size(); ++i) {
        ++end;
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
            ++end;
        }
    }
    return env->NewStringUTF(std::string(text.substr(begin, end - begin)).c_str());
}

// The selected text, or "" when the selection is collapsed. The platform uses it for cut/copy.
JNIEXPORT jstring JNICALL Java_dev_tessera_app_TesseraView_nativeSelectedText(JNIEnv* env, jclass,
                                                                              jlong handle) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr) {
        return env->NewStringUTF("");
    }
    const std::string_view selected = shell->editor.buffer().selected_text();
    return env->NewStringUTF(std::string(selected).c_str());
}

// Moves the caret or extends the selection, in UTF-16 units from the start of the buffer. The
// platform calls this when the user drags the IME's selection handles or the IME itself moves the
// caret; without it the engine's caret and the platform's disagree and the next commit lands in the
// wrong place.
JNIEXPORT void JNICALL Java_dev_tessera_app_TesseraView_nativeSetSelection(JNIEnv*, jclass,
                                                                           jlong handle,
                                                                           jint start_utf16,
                                                                           jint end_utf16) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr) {
        return;
    }
    const std::string_view text = shell->editor.buffer().text();
    // UTF-16 offset to UTF-8 byte offset, walking the buffer once per endpoint.
    auto byte_offset = [&text](jint utf16) -> std::size_t {
        if (utf16 <= 0) {
            return 0;
        }
        std::size_t bytes = 0;
        jint units = 0;
        while (bytes < text.size() && units < utf16) {
            const unsigned char lead = static_cast<unsigned char>(text[bytes]);
            std::size_t length = 1;
            if ((lead & 0xF0U) == 0xF0U) {
                length = 4;
            } else if ((lead & 0xE0U) == 0xE0U) {
                length = 3;
            } else if ((lead & 0xC0U) == 0xC0U) {
                length = 2;
            }
            bytes += length;
            // A 4-byte sequence is two UTF-16 units (a surrogate pair); everything else is one.
            units += length == 4 ? 2 : 1;
        }
        return bytes;
    };
    const std::size_t anchor = byte_offset(start_utf16);
    const std::size_t focus = byte_offset(end_utf16);
    shell->editor.buffer().set_selection(anchor, focus);
    shell->editor_runtime.publish_decoration();
    shell->window->request_redraw();
}

// One Java `KeyEvent`, forwarded to the edit controller. This is the path a hardware key and an
// IME-synthesised key both take: the shell is a Java activity, so neither arrives as the NDK's
// `AInputEvent`, and without this the engine never sees a Backspace at all.
//
// The translation is the pure `translate_key_sample` (unit-tested on every lane); this only reads
// the Java getters. A key the engine has no logical code for produces nothing and the caller lets
// the platform handle it, which is what keeps the system Back gesture working.
JNIEXPORT jboolean JNICALL Java_dev_tessera_app_TesseraView_nativeKey(JNIEnv*, jclass, jlong handle,
                                                                      jint keycode, jint meta_state,
                                                                      jint action,
                                                                      jint repeat_count,
                                                                      jlong event_time_nanos) {
    Shell* shell = shell_from(handle);
    if (shell == nullptr || shell->window == nullptr) {
        return JNI_FALSE;
    }
    tessera::hal::android::KeySample sample;
    sample.keycode = keycode;
    sample.meta_state = meta_state;
    sample.action = action;
    sample.repeat_count = repeat_count;
    sample.timestamp_s = static_cast<double>(event_time_nanos) / 1.0e9;
    const tessera::hal::android::KeyTranslation out =
        tessera::hal::android::translate_key_sample(sample);
    if (out.count == 0) {
        return JNI_FALSE;
    }
    // A key-down that types a character produces two events (the key and the text). Both are
    // offered to the controller in order; the key event is what a Backspace needs, the text event
    // is what a letter needs. The return value reports whether the controller consumed any of them,
    // so the platform keeps its own handling for a key the field ignored.
    bool consumed = false;
    for (std::size_t i = 0; i < out.count; ++i) {
        consumed = dispatch_to_editor(*shell, out.events[i]) || consumed;
    }
    return consumed ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
