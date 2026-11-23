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
void place_caret_from_touch(Shell& shell, float logical_x, float logical_y);

// Extends the selection to a touch, in LOGICAL pixels. The anchor stays where the long-press put
// it, so a drag grows the selection from that point. A point outside the field keeps the selection
// the caller already has rather than collapsing it.
void extend_selection_from_touch(Shell& shell, float logical_x, float logical_y);

} // namespace tessera::android_shell
