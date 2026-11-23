// SPDX-License-Identifier: MIT
//
// The caret seam's declarations (issue #189, docs/guides/android-lane.md section 3.13).
//
// `jni_bridge.cpp` owns the surface, metrics, fold and touch seams; `caret_bridge.cpp` owns the
// caret's box arithmetic and the blink policy. Both need the same `Shell`, and neither may include
// the other's translation unit, so the three entry points live here.

#pragma once

#include "android_caret_translation.hpp"
#include "shell.hpp"

namespace tessera::android_shell {

// The caret-blink policy the driver asks for every iteration: the field blinks only while it holds
// scene focus and is editable with a collapsed selection, which is exactly what
// `EditController::caret_blink_active` decides. Reading the node's pseudo state is a bit test, so
// asking once per iteration costs nothing.
bool caret_active_for_shell(void* user);

// The field's painted box and text facts, in the logical pixels the layout produced. Returns false
// when the shell has no field or no face, so the caller leaves the caret alone rather than jumping
// it to the start.
bool caret_field_for_shell(Shell& shell, tessera::hal::android::CaretField& field);

// Places the caret at a touch, in LOGICAL pixels. A point outside the field leaves the caret alone;
// a point inside it - including in the padding - resolves to the nearest caret.
//
// Returns true when the touch landed on the field. That is the "focused text field" signal the
// platform needs: a custom-rendered field cannot focus itself the way an `EditText` does, so the
// view has to ask the engine whether the tap was on the field before it shows the keyboard. Without
// the answer the keyboard appears on every tap anywhere in the window.
[[nodiscard]] bool place_caret_from_touch(Shell& shell, float logical_x, float logical_y);

// Extends the selection to a touch, in LOGICAL pixels. The anchor stays where the long-press put
// it, so a drag grows the selection from that point. A point outside the field keeps the selection
// the caller already has rather than collapsing it.
void extend_selection_from_touch(Shell& shell, float logical_x, float logical_y);

// The platform's Copy/Cut/Paste (docs/guides/android-lane.md section 3.16). A phone has no Ctrl
// key, so the IME's context menu and the platform's text toolbar call these instead of the engine's
// key chords. Each returns false when it changed nothing - no selection, an empty clipboard, an
// unavailable backend - so the platform can leave its own affordance alone.
bool clipboard_copy(Shell& shell);
bool clipboard_cut(Shell& shell);
bool clipboard_paste(Shell& shell);

// Selects the whole buffer. The platform's "Select all" action reaches this; a phone has no Ctrl+A.
// Returns false when the buffer is already fully selected, so the platform can leave its own
// affordance alone.
bool select_all(Shell& shell);

// The selection's state, for the platform's floating toolbar (docs/guides/android-lane.md
// section 3.17). A phone has no Ctrl key, so a long-press normally raises an `ActionMode` with
// Copy/Cut/Paste/Select all; the platform needs to know whether a selection exists and where to
// anchor the toolbar.
struct SelectionState {
    // True when the buffer has a non-collapsed selection. The toolbar is only shown for one: a
    // collapsed caret has nothing to copy.
    bool has_selection = false;
    // The selection's top-left corner in LOGICAL pixels, relative to the window. The platform
    // converts it to physical pixels for the toolbar anchor.
    float anchor_x = 0.0f;
    float anchor_y = 0.0f;
};

[[nodiscard]] SelectionState selection_state(Shell& shell);

// The caret's box in LOGICAL pixels, relative to the window, for the IME's `requestCursorUpdates`
// (docs/guides/android-lane.md section 3.18). The IME places its suggestion strip and its
// autocorrect highlight from this, so it must be the geometry paint draws - which is why both come
// from `paint::detail::caret_box`.
//
// Returns false when the shell has no field or no face, so the caller leaves the IME's cursor
// anchor alone rather than reporting a box at the origin.
struct CaretGeometry {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

[[nodiscard]] bool caret_geometry(Shell& shell, CaretGeometry& out);

} // namespace tessera::android_shell
