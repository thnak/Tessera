// SPDX-License-Identifier: MIT

#include "tessera/ui/text/text_input_runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace tessera::ui::text {
namespace {

void copy_bounded_text(Event& event, std::string_view value) {
    const std::size_t size = std::min<std::size_t>(value.size(), event.text.size());
    std::copy_n(value.data(), size, event.text.begin());
    event.text_size = static_cast<std::uint32_t>(size);
    event.text_truncated = value.size() > size;
}

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

void TextInputRuntime::attach(Node* node) {
    target_ = node;
    // Attaching must not replay edits that happened before the bind: start from the current count.
    seen_committed_edits_ = controller_->committed_edit_count();
}

bool TextInputRuntime::handle_event(Event& event) {
    if (target_ == nullptr || !is_text_input_event(event.type)) {
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
    return consumed;
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
    // Keep the bounded inline copy for recording sinks and diagnostics.
    copy_bounded_text(change, value);
    // Half-open UTF-8 byte offsets (the units PaintData::TextDecoration consumes).
    change.selection_start = clamp_offset(buffer.selection_start());
    change.selection_end = clamp_offset(buffer.selection_end());

    dispatch_event_to(*target_, change);
    last_change_handled_ = change.handled || target_->has_event_binding(EventType::TextChange);
    ++change_events_;
}

} // namespace tessera::ui::text
