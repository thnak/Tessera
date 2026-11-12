// SPDX-License-Identifier: MIT
//
// IME pre-edit plumbing for the X11/XIM path: the XIMPreeditStart/Draw/Done callbacks splice the
// method's pre-edit into the bounded buffer and drive the pure composition state machine.

#include "x11_xi2.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tessera::hal {

void X11Xi2Pump::handle_preedit_start() {
    preedit_size_ = 0;
    commit_pending_ = false;
    push_all(composition_.start({}, 0));
}

void X11Xi2Pump::handle_preedit_draw(int caret, int change_first, int change_length,
                                     std::string_view text) {
    // XIMPreeditDraw carries a splice, not the whole pre-edit: replace [change_first,
    // change_first + change_length) with `text`. Positions are UTF-8 byte offsets in the multibyte
    // form; anything out of range clamps into the buffer, and the splice stays bounded.
    std::size_t first = change_first > 0 ? static_cast<std::size_t>(change_first) : 0U;
    first = std::min(first, preedit_size_);
    std::size_t length = change_length > 0 ? static_cast<std::size_t>(change_length) : 0U;
    length = std::min(length, preedit_size_ - first);

    // Each segment is copied as a bulk `min(room, available)` move rather than a `spliced[size++]`
    // loop: GCC 13 at -O3 cannot prove the break guard bounds the post-increment store and rejects
    // the loop form as stringop-overflow. The bulk form states the bound the compiler needs.
    std::array<char, x11::kMaxPreeditBytes> spliced{};
    std::size_t size = 0;
    const auto append = [&spliced, &size](const char* data, std::size_t count) {
        const std::size_t copied = std::min(count, spliced.size() - size);
        for (std::size_t index = 0; index < copied; ++index) {
            spliced[size + index] = data[index];
        }
        size += copied;
    };

    append(preedit_.data(), std::min(first, preedit_size_));
    append(text.data(), text.size());
    append(preedit_.data() + std::min(first + length, preedit_size_),
           preedit_size_ - std::min(first + length, preedit_size_));
    preedit_ = spliced;
    preedit_size_ = size;

    const std::string_view preedit(preedit_.data(), preedit_size_);
    const int clamped_caret = caret > 0 ? caret : 0;
    push_all(composition_.update(preedit, static_cast<std::uint32_t>(clamped_caret)));
}

void X11Xi2Pump::handle_preedit_done() {
    // The method is done with the pre-edit. If it still holds text, that text is what committed and
    // the state machine emits the EditText event here; the protocol also reports the same string as
    // a key event with keycode 0, which consume_commit() drops. A method that cleared its pre-edit
    // before this callback commits nothing here, and the key event carries the text instead.
    const std::string_view preedit = composition_.preedit();
    committed_size_ = std::min(preedit.size(), committed_.size());
    for (std::size_t index = 0; index < committed_size_; ++index) {
        committed_[index] = preedit[index];
    }
    const x11::CompositionOutput output = composition_.commit();
    bool has_edit_text = false;
    for (std::size_t index = 0; index < output.count; ++index) {
        if (output.events[index].type == ui::InputEventType::EditText) {
            has_edit_text = true;
        }
        push(output.events[index]);
    }
    preedit_size_ = 0;
    commit_pending_ = has_edit_text;
}

bool X11Xi2Pump::consume_commit(std::string_view text) {
    if (!commit_pending_) {
        return false;
    }
    commit_pending_ = false;
    return text.size() == committed_size_ &&
           std::string_view(committed_.data(), committed_size_) == text;
}

bool X11Xi2Pump::input_method_supports_preedit(XIM input_method) const {
#if !defined(TESSERA_X11_XI2)
    (void)input_method;
    return false;
#else
    if (!enabled_ || input_method == nullptr) {
        return false;
    }
    XIMStyles* styles = nullptr;
    if (XGetIMValues(input_method, XNQueryInputStyle, &styles, static_cast<char*>(nullptr)) !=
            nullptr ||
        styles == nullptr) {
        return false;
    }
    bool supported = false;
    for (unsigned short index = 0; index < styles->count_styles; ++index) {
        if (styles->supported_styles[index] == static_cast<XIMStyle>(XIMPreeditCallbacks)) {
            supported = true;
            break;
        }
    }
    XFree(styles);
    return supported;
#endif
}

bool X11Xi2Pump::attach_input_context(XIM input_method, XIC context) {
#if !defined(TESSERA_X11_XI2)
    (void)input_method;
    (void)context;
    return false;
#else
    (void)input_method;
    if (!enabled_ || context == nullptr) {
        return false;
    }
    XIMCallback start{};
    start.client_data = reinterpret_cast<XPointer>(this);
    start.callback = &X11Xi2Pump::preedit_start_callback;
    XIMCallback draw{};
    draw.client_data = reinterpret_cast<XPointer>(this);
    draw.callback = &X11Xi2Pump::preedit_draw_callback;
    XIMCallback done{};
    done.client_data = reinterpret_cast<XPointer>(this);
    done.callback = &X11Xi2Pump::preedit_done_callback;
    // XNPreeditCaretCallback is intentionally not installed: the draw callback already carries the
    // caret, and a method that wants caret control can keep using the draw position.
    if (XSetICValues(context, XNPreeditStartCallback, &start, XNPreeditDrawCallback, &draw,
                     XNPreeditDoneCallback, &done, static_cast<char*>(nullptr)) != nullptr) {
        return false;
    }
    preedit_attached_ = true;
    return true;
#endif
}

#if defined(TESSERA_X11_XI2)
void X11Xi2Pump::preedit_start_callback(XIM /*context*/, XPointer client_data,
                                        XPointer /*call_data*/) {
    auto* self = reinterpret_cast<X11Xi2Pump*>(client_data);
    if (self != nullptr) {
        self->handle_preedit_start();
    }
}

void X11Xi2Pump::preedit_draw_callback(XIM /*context*/, XPointer client_data, XPointer call_data) {
    auto* self = reinterpret_cast<X11Xi2Pump*>(client_data);
    if (self == nullptr || call_data == nullptr) {
        return;
    }
    const auto* draw = reinterpret_cast<const XIMPreeditDrawCallbackStruct*>(call_data);
    std::string_view text;
    if (draw->text != nullptr && draw->text->encoding_is_wchar == 0 &&
        draw->text->string.multi_byte != nullptr) {
        text = std::string_view(draw->text->string.multi_byte, draw->text->length);
    }
    // A wide-character pre-edit is not converted: UTF-8 locales (the only ones this window
    // requests) deliver the multibyte form, and dropping an unexpected wide string is a clamp, not
    // a crash. Documented in docs/design/input.md.
    self->handle_preedit_draw(draw->caret, draw->chg_first, draw->chg_length, text);
}

void X11Xi2Pump::preedit_done_callback(XIM /*context*/, XPointer client_data,
                                       XPointer /*call_data*/) {
    auto* self = reinterpret_cast<X11Xi2Pump*>(client_data);
    if (self != nullptr) {
        self->handle_preedit_done();
    }
}

#endif // TESSERA_X11_XI2

} // namespace tessera::hal
