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

namespace {

// The advance seam `hit_test_point` takes, backed by the same source paint measures with. Without
// it the hit test falls back to the deterministic stub, whose advances do not match the real face:
// a tap in the middle of "hello" then resolves to the end of the line, and a drag selects nothing.
float advance_from_paint_source(std::string_view run, const tessera::ui::text::TextStyle& style,
                                const void* user) {
    const auto* source = static_cast<const tessera::ui::paint::PaintTextMetricsSource*>(user);
    return source->advance(run, style);
}

} // namespace

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
        const auto tick_action = shell->selection_gesture.tick(now, config);
        if (tick_action == tessera::hal::android::SelectionGestureAction::BeginSelection) {
            // The anchor is the press point, which the state machine kept. Placing it collapses the
            // selection to a caret; the caller's drag then extends from there. A long-press with no
            // drag leaves a caret, which is what every platform does before the finger moves.
            (void)tessera::android_shell::place_caret_from_touch(*shell, shell->selection_press_x,
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
    // `world_bounds`, not `bounds`: a node's `bounds` are in its PARENT's coordinate space, and the
    // touch arrives in the window's. Using the parent-relative box put the field 24 px left of
    // where it paints, so a tap in the middle of a word resolved past the end of the line.
    const auto& bounds = node.world_bounds();
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
    // The same advance source paint measures with, so a tap lands on the glyph under the finger.
    if (shell.field_advance != nullptr) {
        field.advance = &advance_from_paint_source;
        field.advance_user = shell.field_advance.get();
    }
    // The demo field is multi-line, so it wraps at its content width exactly as paint does.
    field.wrap = true;
    return true;
}

bool place_caret_from_touch(Shell& shell, float logical_x, float logical_y) {
    tessera::hal::android::CaretField field;
    if (!caret_field_for_shell(shell, field)) {
        return false;
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
        return false;
    }
    (void)shell.editor.place_caret_at(caret.hit);
    shell.editor_runtime.publish_decoration();
    if (shell.window != nullptr) {
        shell.window->request_redraw();
    }
    return true;
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

bool clipboard_copy(Shell& shell) {
    if (shell.clipboard == nullptr) {
        return false;
    }
    const tessera::ui::text::EditController::ClipboardResult result =
        shell.editor.copy(*shell.clipboard);
    if (!result.changed) {
        // No selection, or the backend is unavailable: the platform's Copy stays a no-op rather
        // than reporting success and leaving the clipboard untouched.
        return false;
    }
    shell.editor_runtime.publish_decoration();
    if (shell.window != nullptr) {
        shell.window->request_redraw();
    }
    return true;
}

bool clipboard_cut(Shell& shell) {
    if (shell.clipboard == nullptr) {
        return false;
    }
    const tessera::ui::text::EditController::ClipboardResult result =
        shell.editor.cut(*shell.clipboard);
    if (!result.changed) {
        return false;
    }
    shell.editor_runtime.publish_decoration();
    if (shell.window != nullptr) {
        shell.window->request_redraw();
    }
    return true;
}

bool clipboard_paste(Shell& shell) {
    if (shell.clipboard == nullptr) {
        return false;
    }
    const tessera::ui::text::EditController::ClipboardResult result =
        shell.editor.paste(*shell.clipboard);
    if (!result.changed) {
        // An empty clipboard, an unavailable backend, or a read-only field: the platform's Paste
        // stays a no-op instead of reporting success.
        return false;
    }
    shell.editor_runtime.publish_decoration();
    if (shell.window != nullptr) {
        shell.window->request_redraw();
    }
    return true;
}

} // namespace tessera::android_shell
