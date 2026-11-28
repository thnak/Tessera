// SPDX-License-Identifier: MIT
//
// The canvas session's text entry points (issue #601, docs/design/canvas-text-input.md): committed
// text, IME composition and the clipboard reads. They feed the focused `TextInput`/`TextArea`
// through `PreviewSession::dispatch`, the same router path a native window uses, so there is one
// editing implementation. Split out of `scene_canvas_session.cpp`.

#include "scene_canvas_session.hpp"
#include "tessera/ui/key_code.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace tessera::compiler::codegen::cli {
namespace {

// `ui::InputEvent` carries at most this many payload bytes; a longer paste goes in several events.
constexpr std::size_t kChunkBytes = 192;

// The length of the longest prefix of `text` within `limit` bytes that ends on a code point
// boundary.
std::size_t chunk_size(std::string_view text, std::size_t limit) {
    if (text.size() <= limit) {
        return text.size();
    }
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0U) == 0x80U) {
        --end;
    }
    return end;
}

} // namespace

CanvasPointerResult CanvasSceneSession::text(PreviewInput::Kind kind, std::string_view text) {
    CanvasPointerResult result;
    if (!started()) {
        return result;
    }
    fill_focus(result);
    const bool commit = kind == PreviewInput::Kind::Text;
    if (!result.field.focused || (commit && text.empty())) {
        return result;
    }
    // A preedit is one payload and is cut, a commit is split so no byte is lost.
    std::size_t offset = 0;
    bool consumed = false;
    do {
        const std::string_view rest = text.substr(offset);
        const std::size_t size = chunk_size(rest, kChunkBytes);
        PreviewInput input;
        input.kind = kind;
        input.text.assign(rest.substr(0, size));
        result.dispatched = session_->dispatch(input) || result.dispatched;
        consumed = session_->text_consumed() || consumed;
        result.ran.insert(result.ran.end(), session_->last_ran().begin(),
                          session_->last_ran().end());
        result.unrun.insert(result.unrun.end(), session_->last_unrun().begin(),
                            session_->last_unrun().end());
        offset += size;
        if (size == 0) {
            break;
        }
    } while (commit && offset < text.size());
    fill_focus(result);
    result.handled = consumed;
    result.changed = needs_frame();
    return result;
}

CanvasPointerResult CanvasSceneSession::text_clipboard(bool cut) {
    CanvasPointerResult result;
    if (!started()) {
        return result;
    }
    std::string selected = session_->focused_text_selection();
    if (selected.empty()) {
        fill_focus(result);
        return result;
    }
    if (cut) {
        // Deleting the selection is what Backspace does with one, undo step included.
        result = key(PreviewInput::Kind::KeyDown, ui::key::kBackspace, 0U, false);
    } else {
        fill_focus(result);
    }
    result.has_text = true;
    result.text = std::move(selected);
    result.handled = true;
    return result;
}

} // namespace tessera::compiler::codegen::cli
