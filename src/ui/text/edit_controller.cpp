// SPDX-License-Identifier: MIT

#include "edit_grapheme.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/text/edit_controller.hpp"

#include <algorithm>
#include <string_view>

namespace tessera::ui::text {
namespace {

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80U) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp <= 0x10FFFFU) {
        out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
}

std::string_view event_text(const Event& event) {
    const std::size_t size = std::min<std::size_t>(event.text_size, kMaxEditTextBytes);
    return std::string_view(event.text.data(), size);
}

} // namespace

EditController::EditController(std::size_t max_bytes)
    : buffer_(max_bytes) {}

EditResult EditController::set_text(std::string_view text) {
    composing_ = false;
    preedit_.clear();
    const EditResult result = buffer_.set_text(text);
    last_result_ = result;
    changed_ = changed_ || result.changed;
    return result;
}

bool EditController::commit_text(std::string_view text) {
    if (composing_) {
        const EditOffset start = composition_start_;
        const EditOffset end = composition_end_;
        composing_ = false;
        preedit_.clear();
        return accept(buffer_.replace_range(start, end, text), true);
    }
    return accept(buffer_.insert(text), true);
}

bool EditController::accept(EditResult result, bool text_edit) {
    last_result_ = result;
    changed_ = changed_ || result.changed;
    if (text_edit && result.changed) {
        ++committed_edits_;
    }
    return true;
}

bool EditController::handle_event(Event& event) {
    if (event.prevent_default || event.handled) {
        return false;
    }
    switch (event.type) {
    case EventType::Text: {
        if (read_only_ || composing_) {
            return false;
        }
        const std::uint32_t cp = event.text_codepoint;
        if (cp == 0 || cp == '\r' || (cp < 0x20U && cp != '\n')) {
            return false;
        }
        if (cp == '\n' && !multiline_) {
            return false;
        }
        std::string bytes;
        append_utf8(bytes, cp);
        event.handled = true;
        return accept(buffer_.insert(bytes), true);
    }
    case EventType::EditText: {
        if (read_only_) {
            return false;
        }
        event.handled = true;
        return commit_text(event_text(event));
    }
    case EventType::CompositionStart: {
        if (read_only_ || composing_) {
            return false;
        }
        composing_ = true;
        preedit_.clear();
        composition_start_ = buffer_.selection_start();
        composition_end_ = buffer_.selection_end();
        event.handled = true;
        return true;
    }
    case EventType::CompositionUpdate: {
        if (read_only_ || !composing_) {
            return false;
        }
        const std::string_view text = event_text(event);
        if (!edit_detail::valid_utf8(text)) {
            return false;
        }
        preedit_.assign(text.data(), text.size());
        event.handled = true;
        return true;
    }
    case EventType::CompositionEnd: {
        if (!composing_) {
            return false;
        }
        // No committed `EditText` arrived: cancel restores the pre-composition state.
        composing_ = false;
        preedit_.clear();
        event.handled = true;
        return true;
    }
    case EventType::KeyDown:
        return handle_key(event);
    default:
        return false;
    }
}

bool EditController::handle_key(const Event& event) {
    const std::uint32_t key = event.key;
    const bool control = has_modifier(event.modifiers, Modifier::Control);
    const bool shift = has_modifier(event.modifiers, Modifier::Shift);

    if (key == key::kBackspace || key == key::kDelete) {
        if (read_only_) {
            return false;
        }
        return accept(key == key::kBackspace ? buffer_.delete_backward(control)
                                             : buffer_.delete_forward(control),
                      true);
    }
    if (key == key::kArrowLeft || key == key::kArrowRight) {
        return accept(key == key::kArrowLeft ? buffer_.move_left(control, shift)
                                             : buffer_.move_right(control, shift));
    }
    if (key == key::kHome || key == key::kEnd) {
        return accept(key == key::kHome ? buffer_.move_home(control, shift)
                                        : buffer_.move_end(control, shift));
    }
    if (control && key == 'A') {
        return accept(buffer_.select_all());
    }
    if (key == key::kEnter) {
        if (read_only_ || !multiline_) {
            return false;
        }
        return accept(buffer_.insert("\n"), true);
    }
    return false;
}

} // namespace tessera::ui::text
