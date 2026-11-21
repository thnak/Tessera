// SPDX-License-Identifier: MIT

#include "tessera/ui/behavior/hotkeys.hpp"
#include "tessera/ui/input/key_bindings.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"

#include <string_view>

namespace tessera::ui::behavior {
namespace {

// The manifest's option order: `chords`, `allowInTextInput`, `repeat`.
constexpr std::string_view kOptionNames[] = {"chords", "allowInTextInput", "repeat"};

// The accelerator callback the registry invokes. It does nothing: the behavior's job is to make the
// chord *fire*, and the component's handler is what runs. A behavior may not invoke a callback
// (behavior-hooks-v1.md section 8), so the registry entry is a marker the input path reports
// through `InputDispatchResult::accelerator_fired`, which the generated base turns into the handler
// call.
void no_op_accelerator(void* /*user*/, const InputEvent& /*event*/) {}

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ignoring_case(std::string_view text, std::string_view name) {
    if (text.size() != name.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (ascii_lower(text[i]) != ascii_lower(name[i])) {
            return false;
        }
    }
    return true;
}

// A named non-printable key, or 0 when the name is not one. The names are the `key_code.hpp`
// constants without their `k` prefix, lower-cased.
std::uint32_t named_key(std::string_view name) {
    struct Entry {
        std::string_view name;
        std::uint32_t code;
    };
    static constexpr Entry kNames[] = {
        {"backspace", key::kBackspace},
        {"tab", key::kTab},
        {"enter", key::kEnter},
        {"escape", key::kEscape},
        {"space", key::kSpace},
        {"insert", key::kInsert},
        {"delete", key::kDelete},
        {"home", key::kHome},
        {"end", key::kEnd},
        {"pageup", key::kPageUp},
        {"pagedown", key::kPageDown},
        {"arrowleft", key::kArrowLeft},
        {"arrowup", key::kArrowUp},
        {"arrowright", key::kArrowRight},
        {"arrowdown", key::kArrowDown},
    };
    for (const Entry& entry : kNames) {
        if (equals_ignoring_case(name, entry.name)) {
            return entry.code;
        }
    }
    // `f1`..`f12`.
    if (name.size() >= 2 && ascii_lower(name[0]) == 'f') {
        std::uint32_t number = 0;
        bool digits = true;
        for (std::size_t i = 1; i < name.size(); ++i) {
            if (name[i] < '0' || name[i] > '9') {
                digits = false;
                break;
            }
            number = number * 10 + static_cast<std::uint32_t>(name[i] - '0');
        }
        if (digits && number >= 1 && number <= 12) {
            return key::kF1 + (number - 1);
        }
    }
    return 0;
}

} // namespace

Hotkeys::~Hotkeys() {
    // A behavior destroyed without `detach` must not leave a chord registered: the window would
    // keep firing a shortcut for a component that no longer exists.
    detach();
}

bool Hotkeys::parse_chord(std::string_view text, Chord& out) {
    out = Chord{};
    if (text.empty()) {
        return false;
    }
    // Split on `+`. The last segment is the key; every earlier one is a modifier.
    std::size_t start = 0;
    std::string_view key_name;
    while (true) {
        const std::size_t plus = text.find('+', start);
        const std::string_view segment = text.substr(
            start, plus == std::string_view::npos ? std::string_view::npos : plus - start);
        if (segment.empty()) {
            return false;
        }
        if (plus == std::string_view::npos) {
            key_name = segment;
            break;
        }
        if (equals_ignoring_case(segment, "ctrl") || equals_ignoring_case(segment, "control")) {
            out.modifiers |= static_cast<std::uint32_t>(Modifier::Control);
        } else if (equals_ignoring_case(segment, "alt")) {
            out.modifiers |= static_cast<std::uint32_t>(Modifier::Alt);
        } else if (equals_ignoring_case(segment, "shift")) {
            out.modifiers |= static_cast<std::uint32_t>(Modifier::Shift);
        } else if (equals_ignoring_case(segment, "meta") || equals_ignoring_case(segment, "cmd") ||
                   equals_ignoring_case(segment, "super")) {
            out.modifiers |= static_cast<std::uint32_t>(Modifier::Meta);
        } else {
            return false; // an unknown modifier
        }
        start = plus + 1;
    }
    if (key_name.size() == 1) {
        const char c = key_name[0];
        if (c >= 'a' && c <= 'z') {
            out.key = static_cast<std::uint32_t>(c - 'a' + 'A');
            return true;
        }
        if (c >= 'A' && c <= 'Z') {
            out.key = static_cast<std::uint32_t>(c);
            return true;
        }
        if (c >= '0' && c <= '9') {
            out.key = static_cast<std::uint32_t>(c);
            return true;
        }
        return false;
    }
    out.key = named_key(key_name);
    return out.key != 0;
}

void Hotkeys::attach(const Targets& targets, const Services& services, const ArgsView& args) {
    (void)targets; // a hotkey scope is the window, not an element
    router_ = services.router;
    apply(args);
}

void Hotkeys::update(const ArgsView& args) {
    apply(args);
}

void Hotkeys::detach() {
    release();
    router_ = nullptr;
    outputs_ = {};
}

std::span<const OutputValue> Hotkeys::outputs() const {
    return outputs_;
}

void Hotkeys::release() {
    if (router_ == nullptr) {
        count_ = 0;
        return;
    }
    for (std::size_t i = 0; i < count_; ++i) {
        router_->accelerators().remove(ids_[i]);
    }
    count_ = 0;
}

void Hotkeys::apply(const ArgsView& args) {
    // Re-registering on every update is the simple, correct answer: the option is a list, and a
    // diff would have to compare chords. The registry is bounded and removal is O(1) per id, so a
    // component that re-renders does not accumulate bindings.
    release();
    if (router_ == nullptr) {
        outputs_[0].kind = OutputValue::Kind::Bool;
        outputs_[0].boolean = false;
        return;
    }
    const ArgValue* chords = args.find("chords", kOptionNames);
    const ArgValue* repeat = args.find("repeat", kOptionNames);
    const ArgValue* allow_in_text = args.find("allowInTextInput", kOptionNames);
    const bool allow_repeat =
        repeat != nullptr && repeat->kind == ArgValue::Kind::Bool && repeat->boolean;
    const bool text_ok = allow_in_text != nullptr && allow_in_text->kind == ArgValue::Kind::Bool &&
                         allow_in_text->boolean;
    // A chord that fires while a text field has focus steals a key the user is typing, so the
    // default is to skip registration while one is focused. The registry has no "except when"
    // rule, so the behavior re-evaluates on each update: a component whose text field gains focus
    // re-renders (the field's own state changed) and the chord is withdrawn.
    const bool text_focused = text_input_focused();
    if (text_focused && !text_ok) {
        outputs_[0].kind = OutputValue::Kind::Bool;
        outputs_[0].boolean = false;
        return;
    }
    if (chords != nullptr && chords->kind == ArgValue::Kind::String) {
        // The manifest declares `chords` as a comma-separated list, so one string carries the whole
        // set and the behavior needs no array option kind.
        std::string_view rest = chords->text;
        while (!rest.empty() && count_ < kMaxChords) {
            const std::size_t comma = rest.find(',');
            const std::string_view one =
                rest.substr(0, comma == std::string_view::npos ? std::string_view::npos : comma);
            Chord chord;
            if (parse_chord(one, chord)) {
                const BindingId id = router_->accelerators().add(
                    KeyChord{chord.key, static_cast<Modifier>(chord.modifiers)}, &no_op_accelerator,
                    this, allow_repeat);
                if (id != kInvalidBindingId) {
                    ids_[count_++] = id;
                }
            }
            if (comma == std::string_view::npos) {
                break;
            }
            rest = rest.substr(comma + 1);
        }
    }
    outputs_[0].kind = OutputValue::Kind::Bool;
    outputs_[0].boolean = count_ > 0;
}

bool Hotkeys::text_input_focused() const {
    if (router_ == nullptr) {
        return false;
    }
    const Node* focused = router_->focus_manager().focused();
    if (focused == nullptr) {
        return false;
    }
    const std::string_view kind = focused->element_kind();
    return kind == "TextInput" || kind == "TextArea";
}

} // namespace tessera::ui::behavior
