// SPDX-License-Identifier: MIT
//
// The Android clipboard backend (issue #189, docs/guides/android-lane.md section 3.16).
//
// `hal::Clipboard` is a synchronous seam: `read_text()` returns a value, not a future. Android's
// `ClipboardManager` is also synchronous, but it must be touched on a thread with a `Looper`, and
// the engine's render thread has none. The bridge therefore keeps a cache:
//
//   * a WRITE updates the engine's copy immediately (so a copy-then-paste in the same frame sees
//   the
//     new text) and asks the view to post the platform write to the UI thread;
//   * a READ returns the cached text, which the UI thread refreshes whenever the platform reports a
//     change.
//
// That is the shape every Android text field uses, and it keeps the seam non-blocking: the engine
// never waits on the UI thread, and the UI thread never waits on the engine.

#include "android_clipboard_text.hpp"
#include "tessera/hal/clipboard.hpp"

#include <jni.h>
#include <mutex>
#include <string>

namespace tessera::hal {
namespace {

// The cached clipboard, guarded because the UI thread writes it and the render thread reads it.
std::mutex g_clipboard_mutex;
std::string g_clipboard_text;
bool g_clipboard_available = false;

// The Java view, so a write can ask it to post. Borrowed: the shell owns both and destroys the
// backend before the view.
JavaVM* g_vm = nullptr;
jobject g_view = nullptr;

class AndroidClipboard final : public Clipboard {
public:
    std::expected<std::string, ClipboardError> read_text() override {
        const std::lock_guard lock(g_clipboard_mutex);
        if (!g_clipboard_available) {
            return std::unexpected(ClipboardError::Unsupported);
        }
        if (g_clipboard_text.size() > kMaxClipboardBytes) {
            return std::unexpected(ClipboardError::TooLarge);
        }
        return g_clipboard_text;
    }

    std::expected<void, ClipboardError> write_text(std::string_view text) override {
        if (text.size() > kMaxClipboardBytes) {
            return std::unexpected(ClipboardError::TooLarge);
        }
        {
            const std::lock_guard lock(g_clipboard_mutex);
            if (!g_clipboard_available) {
                return std::unexpected(ClipboardError::Unsupported);
            }
            // The engine's copy first, so a copy-then-paste in the same frame sees the new text
            // even before the UI thread runs the platform write.
            g_clipboard_text.assign(text);
        }
        // The platform write is posted by the view: the render thread has no `Looper`, and a direct
        // `ClipboardManager` call from it throws.
        JNIEnv* env = nullptr;
        if (g_vm == nullptr ||
            g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            return std::unexpected(ClipboardError::Failed);
        }
        if (g_view == nullptr) {
            return std::unexpected(ClipboardError::Failed);
        }
        jclass view_class = env->GetObjectClass(g_view);
        if (view_class == nullptr) {
            return std::unexpected(ClipboardError::Failed);
        }
        jmethodID post_write = env->GetMethodID(view_class, "postClipboardWrite", "()V");
        env->DeleteLocalRef(view_class);
        if (post_write == nullptr) {
            return std::unexpected(ClipboardError::Failed);
        }
        env->CallVoidMethod(g_view, post_write);
        return {};
    }
};

} // namespace

std::unique_ptr<Clipboard> create_native_clipboard() {
    return std::make_unique<AndroidClipboard>();
}

} // namespace tessera::hal

// --- The JNI half ------------------------------------------------------------------------------
//
// `nativeClipboardAttach` installs the cache and the view; `nativeClipboardChanged` refreshes it
// when the platform reports a new clip; `nativeClipboardText` is what the view reads when it posts
// a write.

extern "C" {

JNIEXPORT void JNICALL Java_dev_tessera_app_TesseraView_nativeClipboardAttach(JNIEnv* env, jclass,
                                                                              jlong handle,
                                                                              jobject view) {
    (void)handle;
    env->GetJavaVM(&tessera::hal::g_vm);
    if (tessera::hal::g_view != nullptr) {
        env->DeleteGlobalRef(tessera::hal::g_view);
    }
    tessera::hal::g_view = env->NewGlobalRef(view);
    const std::lock_guard lock(tessera::hal::g_clipboard_mutex);
    tessera::hal::g_clipboard_available = true;
}

JNIEXPORT void JNICALL Java_dev_tessera_app_TesseraView_nativeClipboardChanged(JNIEnv* env, jclass,
                                                                               jstring text) {
    std::string utf8;
    if (text != nullptr) {
        const jsize length = env->GetStringLength(text);
        std::u16string units(static_cast<std::size_t>(length), u'\0');
        if (length > 0) {
            env->GetStringRegion(text, 0, length, reinterpret_cast<jchar*>(units.data()));
        }
        utf8 = tessera::hal::android::utf16_to_utf8(units);
    }
    const std::lock_guard lock(tessera::hal::g_clipboard_mutex);
    tessera::hal::g_clipboard_text = std::move(utf8);
}

// The text the view should write to the platform clipboard, as UTF-16. Called from the UI thread
// when the posted write runs.
JNIEXPORT jstring JNICALL Java_dev_tessera_app_TesseraView_nativeClipboardText(JNIEnv* env,
                                                                               jclass) {
    std::string text;
    {
        const std::lock_guard lock(tessera::hal::g_clipboard_mutex);
        text = tessera::hal::g_clipboard_text;
    }
    const std::u16string units = tessera::hal::android::utf8_to_utf16(text);
    return env->NewString(reinterpret_cast<const jchar*>(units.data()),
                          static_cast<jsize>(units.size()));
}

} // extern "C"
