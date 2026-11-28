// SPDX-License-Identifier: MIT
//
// The caret of a canvas text field (issue #980, docs/design/canvas-text-input.md section 8): where
// it is painted, so the page can open the IME's window there, and where a press or a drag puts it.
// The geometry is paint's (`paint::text_field_caret_box` / `text_field_hit`), resolved against the
// same cascade and metrics the frame paints with, so the box and the hit agree with the pixels.
// Split out of `scene_canvas_session.cpp`, which is at its size budget.

#include "scene_canvas_session.hpp"
#include "tessera/ui/paint/text_field_geometry.hpp"
#include "tessera/ui/style/scene_style.hpp"

namespace tessera::compiler::codegen::cli {

void CanvasSceneSession::set_text_metrics(TextMetricsProvider provider) noexcept {
    text_metrics_ = provider;
}

void CanvasSceneSession::fill_caret_box(CanvasPointerResult& result) const {
    const ui::Node* const focused = session_->router().focus_manager().focused();
    if (!result.field.focused || focused == nullptr) {
        return;
    }
    ui::style::SceneStyleTree styles(session_->root());
    const ui::style::ComputedStyle style = styles.compute(sheet(), *focused);
    const std::optional<ui::paint::CaretBox> box = ui::paint::text_field_caret_box(
        *focused, style, focused->world_bounds(), result.field.ime_offset,
        text_metrics_ != nullptr ? text_metrics_() : nullptr, dpi_scale_);
    if (box) {
        result.field.has_caret_box = true;
        result.field.caret_box = {box->x, box->y, box->width, box->height};
    }
}

bool CanvasSceneSession::text_pointer(PreviewInput::Kind kind, float x, float y) {
    if (kind == PreviewInput::Kind::PointerUp || kind == PreviewInput::Kind::PointerCancel) {
        selecting_text_ = false;
        return false;
    }
    const PreviewTextField field = session_->focused_text_field();
    const bool press = kind == PreviewInput::Kind::PointerDown;
    if (!field.focused || field.composing) {
        selecting_text_ = false; // a press elsewhere ends a selection; a composition owns the caret
        return false;
    }
    if (press) {
        selecting_text_ = x >= field.box.x && x < field.box.x + field.box.width &&
                          y >= field.box.y && y < field.box.y + field.box.height;
    }
    if (!selecting_text_ || (!press && kind != PreviewInput::Kind::PointerMove)) {
        return false;
    }
    const ui::Node* const focused = session_->router().focus_manager().focused();
    ui::style::SceneStyleTree styles(session_->root());
    const ui::style::ComputedStyle style = styles.compute(sheet(), *focused);
    const std::optional<ui::text::TextHit> hit =
        ui::paint::text_field_hit(*focused, style, focused->world_bounds(), x, y,
                                  text_metrics_ != nullptr ? text_metrics_() : nullptr);
    return hit && session_->move_text_caret(hit->byte_offset, !press);
}

} // namespace tessera::compiler::codegen::cli
