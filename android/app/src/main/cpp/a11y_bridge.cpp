// SPDX-License-Identifier: MIT
//
// The native half of the Android accessibility bridge (issue #189): answers the pull-side queries
// `TesseraAccessibilityProvider` makes from the platform's UI thread.
//
// Why a separate translation unit from `jni_bridge.cpp`: that file is already 429 lines and this
// half is a self-contained seam (snapshot build, the packed-slot answer, the action queue). It
// shares the `Shell` type through `shell.hpp` rather than duplicating it.
//
// What crosses the boundary and what does not: Java asks for one virtual view's description and
// receives numbers, strings and flags. No `AccessibilityNodeInfo` is ever built here, and no engine
// type is ever seen by Java. That is the same value-only rule docs/design/bridges.md section 3 asks
// of every bridge.

#include "android_a11y_mapping.hpp"
#include "shell.hpp"
#include "tessera/ui/accessibility/snapshot_provider.hpp"

#include <cstdint>
#include <jni.h>
#include <string>
#include <vector>

namespace {

// The packed slot layout `TesseraAccessibilityProvider` reads. Kept in lockstep with the Java
// constants: flags first, then bounds in 1/64 px, then action count + ids, then child count + ids.
// A mismatch here is a silently wrong screen reader, which is why the test pins the layout.
constexpr int kFlagEnabled = 0;
constexpr int kFlagFocusable = 1;
constexpr int kFlagCheckable = 2;
constexpr int kFlagChecked = 3;
constexpr int kFlagSelected = 4;
constexpr int kFlagEditable = 5;
constexpr int kFlagScrollable = 6;
constexpr int kFlagAccessibilityFocused = 7;
// Derived from the engine action set rather than a separate engine concept: a node that offers
// Press is clickable, and TalkBack gates activation on this flag.
constexpr int kFlagClickable = 8;
constexpr int kFlagLongClickable = 9;
// A collection item reports its position; the platform turns these into `CollectionItemInfo`
// (accessibility spec section 5). The flag is explicit because "an item of an empty list" and "not
// an item" are different answers, and a zero `set_size` would conflate them.
constexpr int kFlagCollectionItem = 10;
constexpr int kFlagCount = 11;
constexpr int kBoundsX = kFlagCount;
constexpr int kBoundsY = kFlagCount + 1;
constexpr int kBoundsW = kFlagCount + 2;
constexpr int kBoundsH = kFlagCount + 3;
constexpr int kPosInSetSlot = kFlagCount + 4;
constexpr int kSetSizeSlot = kFlagCount + 5;
constexpr int kMaxActions = static_cast<int>(tessera::hal::android::kMaxActions);
constexpr int kMaxChildren = static_cast<int>(tessera::ui::a11y::kMaxChildren);
constexpr int kActionCountSlot = kFlagCount + 6;
constexpr int kChildCountSlot = kActionCountSlot + 1 + kMaxActions;
constexpr int kSlotCount = kChildCountSlot + 1 + kMaxChildren;

// Writes one flag as a 0/1 slot.
void set_flag(jlong* slots, int index, bool value) {
    slots[index] = value ? 1 : 0;
}

// Fixed point 1/64 px. A logical position is a float; shipping it as an integer slot in 1/64 px
// keeps a fractional coordinate across the crossing so Java rounds once, not twice.
jlong pack(float value) {
    return static_cast<jlong>(value * 64.0F + (value >= 0.0F ? 0.5F : -0.5F));
}

// Builds a Java string from a bounded view, or null when the view is empty. The snapshot's text
// arrays are NUL-terminated inside a fixed capacity, so the view's length is the string length.
jstring to_jstring(JNIEnv* env, std::string_view text) {
    if (text.empty()) {
        return nullptr;
    }
    return env->NewStringUTF(std::string(text).c_str());
}

// The engine action back to the platform's id: the mapping lives in `android_a11y_mapping.cpp`
// beside the forward direction, so both halves of the round trip are unit-tested on every lane.
tessera::ui::a11y::Action engine_action_for(int platform_action) {
    return tessera::hal::android::engine_action_for(platform_action);
}

} // namespace

extern "C" {

// Fills the caller's `text` (class name, text, description) and `slots` (the packed layout) for one
// virtual view. Returns false when the id is stale, which the Java side answers with null.
JNIEXPORT jboolean JNICALL Java_dev_tessera_app_TesseraAccessibilityProvider_nativeDescribe(
    JNIEnv* env, jclass, jlong handle, jint virtual_view_id, jobjectArray text, jlongArray slots) {
    auto* shell =
        reinterpret_cast<tessera::android_shell::Shell*>(static_cast<std::intptr_t>(handle));
    if (shell == nullptr || shell->a11y == nullptr || text == nullptr || slots == nullptr) {
        return JNI_FALSE;
    }
    tessera::ui::a11y::ProviderAnswer answer;
    if (!shell->a11y->describe(virtual_view_id, answer)) {
        return JNI_FALSE;
    }
    const auto info = tessera::hal::android::describe_node(answer);

    // The three strings, in the order the Java class reads them.
    jstring class_name = to_jstring(env, info.class_name != nullptr ? info.class_name : "");
    jstring node_text = to_jstring(env, info.text);
    jstring description = to_jstring(env, info.description);
    env->SetObjectArrayElement(text, 0, class_name);
    env->SetObjectArrayElement(text, 1, node_text);
    env->SetObjectArrayElement(text, 2, description);
    if (class_name != nullptr) {
        env->DeleteLocalRef(class_name);
    }
    if (node_text != nullptr) {
        env->DeleteLocalRef(node_text);
    }
    if (description != nullptr) {
        env->DeleteLocalRef(description);
    }

    if (env->GetArrayLength(slots) < kSlotCount) {
        return JNI_FALSE;
    }
    std::vector<jlong> packed(static_cast<std::size_t>(kSlotCount), 0);
    set_flag(packed.data(), kFlagEnabled, info.enabled);
    set_flag(packed.data(), kFlagFocusable, info.focusable);
    set_flag(packed.data(), kFlagCheckable, info.checkable);
    set_flag(packed.data(), kFlagChecked, info.checked);
    set_flag(packed.data(), kFlagSelected, info.selected);
    set_flag(packed.data(), kFlagEditable, info.editable);
    set_flag(packed.data(), kFlagScrollable, info.scrollable);
    set_flag(packed.data(), kFlagAccessibilityFocused, answer.focused);
    // Clickable and long-clickable are the platform's separate booleans over one engine action.
    const bool pressable =
        tessera::ui::a11y::has_action(answer.actions, tessera::ui::a11y::Action::Press);
    set_flag(packed.data(), kFlagClickable, pressable);
    set_flag(packed.data(), kFlagLongClickable, pressable);
    packed[kBoundsX] = pack(answer.x);
    packed[kBoundsY] = pack(answer.y);
    packed[kBoundsW] = pack(answer.width);
    packed[kBoundsH] = pack(answer.height);
    set_flag(packed.data(), kFlagCollectionItem, info.is_collection_item);
    packed[kPosInSetSlot] = static_cast<jlong>(info.pos_in_set);
    packed[kSetSizeSlot] = static_cast<jlong>(info.set_size);

    packed[kActionCountSlot] = static_cast<jlong>(info.actions.count);
    for (std::size_t i = 0; i < info.actions.count; ++i) {
        packed[static_cast<std::size_t>(kActionCountSlot + 1) + i] =
            static_cast<jlong>(static_cast<std::int32_t>(info.actions.actions[i]));
    }

    // Children are answered in the same call: a screen reader walks the whole tree on focus, and a
    // second crossing per node would double the cost of that walk.
    std::array<int, tessera::ui::a11y::kMaxChildren> children{};
    const std::size_t child_count = shell->a11y->children_of(virtual_view_id, children);
    packed[static_cast<std::size_t>(kChildCountSlot)] = static_cast<jlong>(child_count);
    for (std::size_t i = 0; i < child_count; ++i) {
        packed[static_cast<std::size_t>(kChildCountSlot + 1) + i] = static_cast<jlong>(children[i]);
    }

    env->SetLongArrayRegion(slots, 0, kSlotCount, packed.data());
    return JNI_TRUE;
}

// The children of the host view or of one node, in the tail of the caller's array. Kept as its own
// entry point because the host view has no node description at all.
JNIEXPORT jint JNICALL Java_dev_tessera_app_TesseraAccessibilityProvider_nativeChildren(
    JNIEnv* env, jclass, jlong handle, jint virtual_view_id, jlongArray slots) {
    auto* shell =
        reinterpret_cast<tessera::android_shell::Shell*>(static_cast<std::intptr_t>(handle));
    if (shell == nullptr || shell->a11y == nullptr || slots == nullptr) {
        return 0;
    }
    if (env->GetArrayLength(slots) < kSlotCount) {
        return 0;
    }
    std::array<int, tessera::ui::a11y::kMaxChildren> children{};
    const std::size_t count = shell->a11y->children_of(virtual_view_id, children);
    std::vector<jlong> tail(static_cast<std::size_t>(kSlotCount), 0);
    for (std::size_t i = 0; i < count; ++i) {
        tail[static_cast<std::size_t>(kChildCountSlot + 1) + i] = static_cast<jlong>(children[i]);
    }
    env->SetLongArrayRegion(slots, 0, kSlotCount, tail.data());
    return static_cast<jint>(count);
}

// The virtual id carrying engine focus, or the host id when nothing is focused.
JNIEXPORT jint JNICALL
Java_dev_tessera_app_TesseraAccessibilityProvider_nativeFocusedId(JNIEnv*, jclass, jlong handle) {
    auto* shell =
        reinterpret_cast<tessera::android_shell::Shell*>(static_cast<std::intptr_t>(handle));
    if (shell == nullptr || shell->a11y == nullptr) {
        return tessera::ui::a11y::kHostVirtualId;
    }
    return shell->a11y->focused_virtual_id();
}

// Queues one platform action for the engine. Returns true when it was accepted, which for the
// platform means "the request is in flight", not "the control has already reacted".
//
// `text` carries the payload `ACTION_SET_TEXT` and `ACTION_SET_SELECTION` bring with them: the
// platform's `AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE` (a `CharSequence`) and
// `ACTION_ARGUMENT_SELECTION_START_INT`/`_END_INT` (two ints, which the Java side renders as the
// engine's own "start,end" request). A null/absent text is the empty payload, which every other
// action passes.
JNIEXPORT jboolean JNICALL Java_dev_tessera_app_TesseraAccessibilityProvider_nativePerformAction(
    JNIEnv* env, jclass, jlong handle, jint virtual_view_id, jint platform_action, jstring text) {
    auto* shell =
        reinterpret_cast<tessera::android_shell::Shell*>(static_cast<std::intptr_t>(handle));
    if (shell == nullptr || shell->a11y == nullptr) {
        return JNI_FALSE;
    }
    const auto action = engine_action_for(platform_action);
    if (action == tessera::ui::a11y::Action::None) {
        return JNI_FALSE;
    }
    // The payload is read only for an action that carries one; a `null` string is the empty
    // payload.
    std::string payload;
    if (text != nullptr) {
        const char* utf = env->GetStringUTFChars(text, nullptr);
        if (utf == nullptr) {
            // An OOM while copying: refuse the request rather than queue it with a truncated
            // payload.
            return JNI_FALSE;
        }
        payload.assign(utf);
        env->ReleaseStringUTFChars(text, utf);
    }
    return shell->a11y->post_action(virtual_view_id, action, payload) ? JNI_TRUE : JNI_FALSE;
}

} // extern "C"
