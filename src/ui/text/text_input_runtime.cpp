// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/text/text_input_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace tessera::ui::text {
namespace {

std::uint32_t clamp_offset(EditOffset offset) {
    return static_cast<std::uint32_t>(
        std::min<EditOffset>(offset, static_cast<EditOffset>(0xFFFFFFFFU)));
}

bool is_text_input_event(EventType type) {
    switch (type) {
    case EventType::Text:
    case EventType::EditText:
    case EventType::CompositionStart:
    case EventType::CompositionUpdate:
    case EventType::CompositionEnd:
    case EventType::KeyDown:
        return true;
    default:
        return false;
    }
}

} // namespace

TextInputRuntime::TextInputRuntime(EditController& controller)
    : controller_(&controller),
      seen_committed_edits_(controller.committed_edit_count()) {}

bool TextInputRuntime::handle_event(const Event& event) {
    Event copy = event;
    return handle_event(copy);
}

void TextInputRuntime::attach(Node* node) {
    target_ = node;
    // Attaching must not replay edits that happened before the bind: start from the current count.
    seen_committed_edits_ = controller_->committed_edit_count();
}

bool TextInputRuntime::handle_event(Event& event) {
    if (target_ == nullptr) {
        return false;
    }
    // Focus/Blur are not part of the text-input event family, but an owner that binds them still
    // wants the caret to appear/disappear: publish and stay passive (never consume).
    if (event.type == EventType::Focus || event.type == EventType::Blur) {
        publish_decoration();
        return false;
    }
    if (!is_text_input_event(event.type)) {
        return false;
    }
    // Text/IME/key events only reach a focused field; the scene routes them to the focused node,
    // but the runtime is also usable directly, so gate on the node's focus bit as well.
    if (!style::has_state(target_->pseudo_state(), style::PseudoState::Focus)) {
        return false;
    }
    const bool consumed = controller_->handle_event(event);
    if (consumed) {
        event.handled = true;
    }
    const std::uint64_t edits = controller_->committed_edit_count();
    if (edits != seen_committed_edits_) {
        seen_committed_edits_ = edits;
        emit_change();
    }
    // Caret/selection/preedit follows every event (typing, caret moves, IME updates) so paint never
    // shows a stale caret. A change handler may run apply_state (which rebuilds PaintData and calls
    // back into publish_decoration), so this runs after the change dispatch.
    publish_decoration();
    return consumed;
}

void TextInputRuntime::publish_decoration() {
    if (target_ == nullptr) {
        return;
    }
    std::optional<PaintData> data = target_->paint_data();
    if (!data.has_value() || data->kind != PaintKind::Text) {
        return;
    }
    const EditBuffer& buffer = controller_->buffer();
    const bool focused = style::has_state(target_->pseudo_state(), style::PseudoState::Focus);
    TextDecoration decoration;
    decoration.caret_byte_offset = static_cast<std::size_t>(buffer.caret());
    decoration.selection_begin = static_cast<std::size_t>(buffer.selection_start());
    decoration.selection_end = static_cast<std::size_t>(buffer.selection_end());
    decoration.caret_visible = controller_->caret_blink_active(focused);
    const EditController::PreeditRange preedit = controller_->preedit_range();
    if (preedit.active) {
        decoration.composition_active = true;
        decoration.preedit_begin = static_cast<std::size_t>(preedit.begin);
        decoration.preedit_end = static_cast<std::size_t>(preedit.end);
        decoration.preedit_text.assign(preedit.text);
    }
    // The painted text follows the buffer. A field whose node was built with an empty payload (the
    // Android shell's, and any generated field that starts empty) would otherwise paint nothing
    // while the buffer filled up: the caret would move over blank space. The comparison is on the
    // whole payload so an unchanged buffer still costs no dirty mark.
    const std::string_view value = buffer.text();
    const bool text_changed = data->text != value;
    if (!text_changed && data->decoration == decoration) {
        return;
    }
    data->text.assign(value);
    data->decoration = std::move(decoration);
    target_->set_paint_data(std::move(*data));
}

void TextInputRuntime::emit_change() {
    const EditBuffer& buffer = controller_->buffer();
    const std::string_view value = buffer.text();

    Event change;
    change.type = EventType::TextChange;
    // The borrowed view points into the edit buffer: it is valid for the duration of this dispatch
    // only. A change handler must not mutate the same controller re-entrantly (that would
    // invalidate the view); copy the value if it must be stored.
    change.text_view = value.data();
    change.text_view_size = static_cast<std::uint32_t>(value.size());
    // The same payload through the general borrowed-text channel, so a sink that reads `text`
    // rather than `text_view` still sees it without the event owning a 256-byte copy.
    change.set_text(value);
    // Half-open UTF-8 byte offsets (the units PaintData::TextDecoration consumes).
    change.selection_start = clamp_offset(buffer.selection_start());
    change.selection_end = clamp_offset(buffer.selection_end());

    dispatch_event_to(*target_, change);
    last_change_handled_ = change.handled || target_->has_event_binding(EventType::TextChange);
    ++change_events_;
}

} // namespace tessera::ui::text
