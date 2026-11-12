// SPDX-License-Identifier: MIT

#include "x11_ime_composition.hpp"
#include "x11_input_translation.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::hal::x11 {
namespace {

// Encodes one scalar value (already validated by next_utf8_codepoint) into UTF-8. Returns the byte
// count written; `out` must hold 4 bytes.
std::size_t encode_utf8(std::uint32_t codepoint, char* out) {
    if (codepoint <= 0x7F) {
        out[0] = static_cast<char>(codepoint);
        return 1;
    }
    if (codepoint <= 0x7FF) {
        out[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint <= 0xFFFF) {
        out[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
    out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
    return 4;
}

} // namespace

std::uint32_t utf8_byte_offset(std::string_view text, std::uint32_t characters) {
    std::size_t position = 0;
    std::uint32_t index = 0;
    while (position < text.size()) {
        if (index == characters) {
            return static_cast<std::uint32_t>(position);
        }
        const std::size_t before = position;
        (void)next_utf8_codepoint(text, position);
        if (position <= before) {
            ++position; // defensive: a decoder that makes no progress must not stall the loop
        }
        ++index;
    }
    return static_cast<std::uint32_t>(text.size());
}

void CompositionState::set_preedit(std::string_view text, std::uint32_t caret_characters) {
    size_ = 0;
    caret_bytes_ = 0;
    truncated_ = false;
    std::size_t position = 0;
    std::uint32_t characters = 0;
    bool caret_set = caret_characters == 0U;
    while (position < text.size()) {
        const std::size_t before = position;
        const std::uint32_t codepoint = next_utf8_codepoint(text, position);
        if (position <= before) {
            ++position; // defensive: never stall on a decoder that makes no progress
            continue;
        }
        char encoded[4] = {};
        const std::size_t length = encode_utf8(codepoint, encoded);
        if (size_ + length > kMaxPreeditBytes) {
            truncated_ = true;
            break;
        }
        if (!caret_set && characters == caret_characters) {
            caret_bytes_ = static_cast<std::uint32_t>(size_);
            caret_set = true;
        }
        for (std::size_t index = 0; index < length; ++index) {
            buffer_[size_ + index] = encoded[index];
        }
        size_ += length;
        ++characters;
    }
    // A caret past the end (or past the truncation point) clamps to the kept bytes.
    if (!caret_set || caret_bytes_ > size_) {
        caret_bytes_ = static_cast<std::uint32_t>(size_);
    }
}

ui::InputEvent CompositionState::payload_event(ui::InputEventType type) const {
    ui::InputEvent event;
    event.type = type;
    event.text_size = static_cast<std::uint32_t>(size_);
    for (std::size_t index = 0; index < size_; ++index) {
        event.text[index] = buffer_[index];
    }
    event.text_truncated = truncated_;
    event.selection_start = caret_bytes_;
    event.selection_end = caret_bytes_;
    return event;
}

CompositionOutput CompositionState::start(std::string_view preedit_utf8,
                                          std::uint32_t caret_characters) {
    // A start while already composing replaces the in-progress pre-edit; the replaced text is
    // dropped without a CompositionEnd and never reaches the committed text.
    set_preedit(preedit_utf8, caret_characters);
    composing_ = true;
    CompositionOutput output;
    output.events[0] = payload_event(ui::InputEventType::CompositionStart);
    output.count = 1;
    return output;
}

CompositionOutput CompositionState::update(std::string_view preedit_utf8,
                                           std::uint32_t caret_characters) {
    const bool was_composing = composing_;
    set_preedit(preedit_utf8, caret_characters);
    composing_ = true;
    CompositionOutput output;
    // A draw before the method announced its start still produces a start so no pre-edit is lost.
    output.events[0] = payload_event(was_composing ? ui::InputEventType::CompositionUpdate
                                                   : ui::InputEventType::CompositionStart);
    output.count = 1;
    return output;
}

CompositionOutput CompositionState::commit() {
    CompositionOutput output;
    if (!composing_) {
        return output; // nothing in progress: no committed text, no end event
    }
    const bool has_text = size_ > 0;
    ui::InputEvent committed;
    if (has_text) {
        committed = payload_event(ui::InputEventType::EditText);
    }
    reset();
    if (has_text) {
        output.events[output.count++] = committed;
    }
    output.events[output.count++] = payload_event(ui::InputEventType::CompositionEnd);
    return output;
}

CompositionOutput CompositionState::cancel() {
    CompositionOutput output;
    if (!composing_) {
        return output;
    }
    reset();
    output.events[0] = payload_event(ui::InputEventType::CompositionEnd);
    output.count = 1;
    return output;
}

void CompositionState::reset() {
    buffer_ = {};
    size_ = 0;
    caret_bytes_ = 0;
    truncated_ = false;
    composing_ = false;
}

} // namespace tessera::hal::x11
