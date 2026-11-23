// SPDX-License-Identifier: MIT
//
// The Android shell's caret seam (issue #189, docs/guides/android-lane.md section 3.13).
//
// Split out of `jni_bridge.cpp` when the point-to-caret mapping landed: the surface, metrics, fold
// and touch seams are one responsibility, and the caret's box arithmetic plus the blink policy are
// another. Both include <jni.h>; neither includes the other.
//
// A custom-rendered text field does not get caret placement for free. The platform reports a touch
// as a point in view coordinates, and the edit controller wants a `TextHit`; the mapping between
// them is the pure `caret_from_point` (unit-tested on every lane), and this file only supplies the
// field's box and the touch.

#include "android_caret_translation.hpp"
#include "caret_bridge.hpp"
#include "shell.hpp"
#include "tessera/runtime/steady_clock.hpp"
#include "tessera/ui/style/pseudo_state.hpp"

#include <string_view>

namespace tessera::android_shell {

bool caret_active_for_shell(void* user) {
    auto* shell = static_cast<Shell*>(user);
    if (shell == nullptr || shell->scene == nullptr || shell->scene->input_label == nullptr) {
        return false;
    }
    // The long-press timeout's other half. A finger that holds still produces no MotionEvents at
    // all, so the state machine cannot see the timeout from `feed()` alone; this callback runs once
    // per driver iteration, which is exactly the tick it needs. Doing it here rather than on a
    // posted Java callback keeps the gesture on the same clock as the caret.
    if (shell->selection_gesture.pressing()) {
        tessera::hal::android::SelectionGestureConfig config;
        config.touch_slop = 8.0f;
        const double now = tessera::runtime::SteadyClock{}.now_seconds();
        if (shell->selection_gesture.tick(now, config) ==
            tessera::hal::android::SelectionGestureAction::BeginSelection) {
            // The anchor is the press point, which the state machine kept.
            tessera::android_shell::place_caret_from_touch(*shell, shell->selection_press_x,
                                                           shell->selection_press_y);
        }
    }
    const bool focused = tessera::ui::style::has_state(shell->scene->input_label->pseudo_state(),
                                                       tessera::ui::style::PseudoState::Focus);
    return shell->editor.caret_blink_active(focused);
}

bool caret_field_for_shell(Shell& shell, tessera::hal::android::CaretField& field) {
    if (shell.scene == nullptr || shell.scene->input_label == nullptr || !shell.field_face ||
        shell.field_metrics == nullptr) {
        return false;
    }
    const tessera::ui::Node& node = *shell.scene->input_label;
    const auto& bounds = node.bounds();
    field.box_x = bounds.x;
    field.box_y = bounds.y;
    field.box_width = bounds.width;
    field.box_height = bounds.height;
    // The padding the stylesheet authored. The scene does not expose a node's resolved insets, and
    // the shell owns the sheet, so the value it wrote is the value it reads back.
    field.pad_left = shell.field_pad;
    field.pad_top = shell.field_pad;
    field.pad_right = shell.field_pad;
    field.pad_bottom = shell.field_pad;
    const auto& paint = node.paint_data();
    field.text = paint.has_value() ? std::string_view(paint->text) : std::string_view{};
    // The style the field paints with. `text_style_from` is the same mapping the measure and paint
    // seams use, so the hit test measures the glyphs that are on screen.
    field.style = &shell.field_text_style;
    field.metrics = &shell.field_font_metrics;
    // The demo field is multi-line, so it wraps at its content width exactly as paint does.
    field.wrap = true;
    return true;
}

void place_caret_from_touch(Shell& shell, float logical_x, float logical_y) {
    tessera::hal::android::CaretField field;
    if (!caret_field_for_shell(shell, field)) {
        return;
    }
    tessera::hal::android::CaretSample sample;
    // The caller passes LOGICAL pixels: `translate_motion_sample` already divided the platform's
    // physical ones by the density, so passing the density again would divide twice and put every
    // caret at the wrong character.
    sample.x = logical_x;
    sample.y = logical_y;
    sample.dpi_scale = 1.0f;
    const tessera::hal::android::CaretTranslation caret =
        tessera::hal::android::caret_from_point(field, sample);
    if (!caret.inside) {
        // A tap outside the field leaves the caret alone instead of jumping it to the start.
        return;
    }
    (void)shell.editor.place_caret_at(caret.hit);
    shell.editor_runtime.publish_decoration();
    if (shell.window != nullptr) {
        shell.window->request_redraw();
    }
}

void extend_selection_from_touch(Shell& shell, float logical_x, float logical_y) {
    tessera::hal::android::CaretField field;
    if (!caret_field_for_shell(shell, field)) {
        return;
    }
    tessera::hal::android::CaretSample sample;
    sample.x = logical_x;
    sample.y = logical_y;
    sample.dpi_scale = 1.0f;
    const tessera::hal::android::CaretTranslation caret =
        tessera::hal::android::caret_from_point(field, sample);
    if (!caret.inside) {
        // A drag that leaves the field keeps the selection it already has rather than collapsing
        // it.
        return;
    }
    (void)shell.editor.extend_selection_to(caret.hit);
    shell.editor_runtime.publish_decoration();
    if (shell.window != nullptr) {
        shell.window->request_redraw();
    }
}

} // namespace tessera::android_shell
