// SPDX-License-Identifier: MIT

#include "tessera/ui/key_code.hpp"
#include "web_input_translation.hpp"

#include <algorithm>
#include <array>
#include <cstddef>

namespace tessera::hal::web {
namespace {

struct NamedKey {
    std::string_view name;
    ui::KeyCode code;
};

// Short enough for a linear scan: consulted per key event, never per frame.
constexpr std::array<NamedKey, 21> kNamedKeys{{
    {"Backspace", ui::key::kBackspace},
    {"Tab", ui::key::kTab},
    {"Enter", ui::key::kEnter},
    {"Escape", ui::key::kEscape},
    {" ", ui::key::kSpace},
    {"Insert", ui::key::kInsert},
    {"Delete", ui::key::kDelete},
    {"Home", ui::key::kHome},
    {"End", ui::key::kEnd},
    {"PageUp", ui::key::kPageUp},
    {"PageDown", ui::key::kPageDown},
    {"ArrowLeft", ui::key::kArrowLeft},
    {"ArrowUp", ui::key::kArrowUp},
    {"ArrowRight", ui::key::kArrowRight},
    {"ArrowDown", ui::key::kArrowDown},
    {"Shift", ui::key::kShift},
    {"Control", ui::key::kControl},
    {"Alt", ui::key::kAlt},
    {"Meta", ui::key::kMeta},
    {"CapsLock", ui::key::kCapsLock},
    {"Spacebar", ui::key::kSpace},
}};

constexpr std::uint32_t kReplacement = 0xFFFD;

// Decodes the single code point `text` holds. Anything that is not exactly one well-formed scalar
// (a name like "Enter", a truncated or overlong sequence, a surrogate) returns false.
bool decode_single(std::string_view text, std::uint32_t& out, bool& malformed) {
    malformed = false;
    if (text.empty()) {
        return false;
    }
    const auto lead = static_cast<unsigned char>(text[0]);
    std::size_t length = 1;
    std::uint32_t value = lead;
    std::uint32_t minimum = 0;
    if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        value = lead & 0x07U;
        minimum = 0x10000;
    } else if (lead >= 0xE0) {
        length = lead < 0xF0 ? 3 : 0;
        value = lead & 0x0FU;
        minimum = 0x800;
    } else if (lead >= 0xC2 && lead < 0xE0) {
        length = 2;
        value = lead & 0x1FU;
        minimum = 0x80;
    } else if (lead >= 0x80) {
        length = 0; // stray continuation or an invalid lead byte
    }
    if (length == 0 || text.size() != length) {
        malformed = text.size() >= 1 && (length == 0 || text.size() < length);
        return false;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if ((byte & 0xC0U) != 0x80U) {
            malformed = true;
            return false;
        }
        value = (value << 6U) | (byte & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        malformed = true;
        return false;
    }
    out = value;
    return true;
}

ui::PointerKind kind_of(DomPointerType type) {
    switch (type) {
    case DomPointerType::Touch:
        return ui::PointerKind::Touch;
    case DomPointerType::Pen:
        return ui::PointerKind::Pen;
    case DomPointerType::Mouse:
    case DomPointerType::Unknown:
        break;
    }
    return ui::PointerKind::Mouse;
}

} // namespace

ui::Modifier modifiers_from_bits(unsigned bits) {
    auto result = ui::Modifier::None;
    if ((bits & kModShift) != 0U) {
        result = result | ui::Modifier::Shift;
    }
    if ((bits & kModControl) != 0U) {
        result = result | ui::Modifier::Control;
    }
    if ((bits & kModAlt) != 0U) {
        result = result | ui::Modifier::Alt;
    }
    if ((bits & kModMeta) != 0U) {
        result = result | ui::Modifier::Meta;
    }
    if ((bits & kModCapsLock) != 0U) {
        result = result | ui::Modifier::CapsLock;
    }
    if ((bits & kModNumLock) != 0U) {
        result = result | ui::Modifier::NumLock;
    }
    return result;
}

ui::PointerButton button_from_dom(int dom_button) {
    switch (dom_button) {
    case -1:
        return ui::PointerButton::None;
    case 0:
        return ui::PointerButton::Left;
    case 1:
        return ui::PointerButton::Middle;
    case 2:
        return ui::PointerButton::Right;
    case 3:
        return ui::PointerButton::Back;
    case 4:
        return ui::PointerButton::Forward;
    default:
        break;
    }
    return ui::PointerButton::Other;
}

ui::KeyCode key_code_from_dom(std::string_view key) {
    for (const auto& named : kNamedKeys) {
        if (named.name == key) {
            return named.code;
        }
    }
    // F1..F12.
    if (key.size() >= 2 && key.size() <= 3 && key[0] == 'F') {
        unsigned number = 0;
        for (const char digit : key.substr(1)) {
            if (digit < '0' || digit > '9') {
                return ui::key::kUnknown;
            }
            number = number * 10U + static_cast<unsigned>(digit - '0');
        }
        if (number >= 1 && number <= 12) {
            return ui::key::kF1 + (number - 1U);
        }
        return ui::key::kUnknown;
    }
    // Letters and digits: the key's meaning in the active layout, upper-cased ASCII.
    if (key.size() == 1) {
        const char c = key[0];
        if (c >= 'a' && c <= 'z') {
            return static_cast<ui::KeyCode>(c - 'a' + 'A');
        }
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            return static_cast<ui::KeyCode>(c);
        }
    }
    return ui::key::kUnknown;
}

std::uint32_t typed_codepoint(std::string_view key, ui::Modifier modifiers) {
    const bool control = ui::has_modifier(modifiers, ui::Modifier::Control);
    const bool alt = ui::has_modifier(modifiers, ui::Modifier::Alt);
    const bool meta = ui::has_modifier(modifiers, ui::Modifier::Meta);
    if (meta || (control && !alt)) {
        return 0;
    }
    std::uint32_t value = 0;
    bool malformed = false;
    if (decode_single(key, value, malformed)) {
        // Control characters arrive as key events, never as text.
        return value < 0x20 || value == 0x7F ? 0U : value;
    }
    // A malformed one-code-point string is an input-method fault: surface it as U+FFFD instead of
    // dropping it silently, but never for a multi-byte name such as "Enter".
    if (malformed && key.size() <= 4 && !key.empty() &&
        static_cast<unsigned char>(key[0]) >= 0x80) {
        return kReplacement;
    }
    return 0;
}

ui::InputEvent make_pointer_event(const DomPointerSample& sample) {
    ui::InputEvent event;
    switch (sample.phase) {
    case DomPointerPhase::Down:
        event.type = ui::InputEventType::PointerDown;
        break;
    case DomPointerPhase::Move:
        event.type = ui::InputEventType::PointerMove;
        break;
    case DomPointerPhase::Up:
        event.type = ui::InputEventType::PointerUp;
        break;
    case DomPointerPhase::Cancel:
        event.type = ui::InputEventType::PointerCancel;
        break;
    }
    event.timestamp_s = sample.timestamp_ms / 1000.0;
    event.x = sample.x;
    event.y = sample.y;
    event.pointer_kind = kind_of(sample.type);
    // A DOM mouse reports a constant non-zero id (Chrome's is 1), but the router treats id 0 as the
    // primary pointer until a press promotes another one, exactly as the native mouse (id 0) does.
    // Carrying the DOM id would leave a mouse unable to hover until its first click.
    event.pointer_id = event.pointer_kind == ui::PointerKind::Mouse ? 0U : sample.pointer_id;
    event.is_primary = sample.primary;
    event.modifiers = modifiers_from_bits(sample.modifiers);
    event.pressure =
        sample.type == DomPointerType::Pen ? std::clamp(sample.pressure, 0.0F, 1.0F) : 0.0F;
    const bool changes_button =
        sample.phase == DomPointerPhase::Down || sample.phase == DomPointerPhase::Up;
    if (!changes_button) {
        event.button = ui::PointerButton::None;
    } else if (sample.type == DomPointerType::Touch && sample.button < 0) {
        // A touch contact is the primary button, like the Win32 translator.
        event.button = ui::PointerButton::Left;
    } else {
        event.button = button_from_dom(sample.button);
    }
    return event;
}

ui::InputEvent make_wheel_event(const DomWheelSample& sample) {
    float per_notch = kPixelsPerNotch;
    float multiplier = 1.0F;
    switch (sample.mode) {
    case DomWheelMode::Pixel:
    case DomWheelMode::Unknown:
        break;
    case DomWheelMode::Line:
        per_notch = kLinesPerNotch;
        break;
    case DomWheelMode::Page:
        per_notch = 1.0F;
        multiplier = kNotchesPerPage;
        break;
    }
    ui::InputEvent event;
    event.type = ui::InputEventType::Wheel;
    event.timestamp_s = sample.timestamp_ms / 1000.0;
    event.x = sample.x;
    event.y = sample.y;
    event.dx = sample.delta_x / per_notch * multiplier;
    event.dy = sample.delta_y / per_notch * multiplier;
    event.modifiers = modifiers_from_bits(sample.modifiers);
    return event;
}

ui::InputEvent make_key_event(const DomKeySample& sample) {
    ui::InputEvent event;
    event.type = sample.down ? ui::InputEventType::KeyDown : ui::InputEventType::KeyUp;
    event.timestamp_s = sample.timestamp_ms / 1000.0;
    event.key = key_code_from_dom(sample.key);
    event.modifiers = modifiers_from_bits(sample.modifiers);
    event.repeat = sample.down && sample.repeat;
    return event;
}

bool make_text_event(const DomKeySample& sample, ui::InputEvent& out) {
    if (!sample.down) {
        return false;
    }
    const std::uint32_t codepoint =
        typed_codepoint(sample.key, modifiers_from_bits(sample.modifiers));
    if (codepoint == 0) {
        return false;
    }
    out = ui::InputEvent{};
    out.type = ui::InputEventType::Text;
    out.timestamp_s = sample.timestamp_ms / 1000.0;
    out.text_codepoint = codepoint;
    out.modifiers = modifiers_from_bits(sample.modifiers);
    return true;
}

} // namespace tessera::hal::web
