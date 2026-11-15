// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "tessera/ui/key_code.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace tessera::ui::text {

// Which editing command a key chord means (docs/architecture/god-type-audit-v1.md section 3.7).
// The controller's `handle_key` was an `if` chain over key code x modifiers; every new chord edited
// that chain and nothing related the chord set to itself. Here the chords are data and the
// dispatcher is one lookup plus a switch over the command.
enum class EditCommand : std::uint8_t {
    None,
    // Buffered mutations: recorded as one undo step.
    DeleteBackward,
    DeleteBackwardWord,
    DeleteForward,
    DeleteForwardWord,
    InsertNewline,
    // Caret and selection moves: accepted, never recorded in the history.
    MoveLeft,
    MoveRight,
    MoveLeftWord,
    MoveRightWord,
    MoveLeftExtend,
    MoveRightExtend,
    MoveLeftWordExtend,
    MoveRightWordExtend,
    MoveHome,
    MoveEnd,
    MoveHomeDocument,
    MoveEndDocument,
    MoveHomeExtend,
    MoveEndExtend,
    MoveHomeDocumentExtend,
    MoveEndDocumentExtend,
    SelectAll,
    // Clipboard and history chords; the dispatcher consumes one only when it changes something.
    Copy,
    Cut,
    Paste,
    Undo,
    Redo,
};

// Modifier bits this table reasons about.
enum class ModifierSet : std::uint8_t {
    None = 0,
    Control = 1U << 0,
    Shift = 1U << 1,
    Alt = 1U << 2,
    Meta = 1U << 3,
};

constexpr ModifierSet operator|(ModifierSet lhs, ModifierSet rhs) {
    return static_cast<ModifierSet>(static_cast<std::uint8_t>(lhs) |
                                    static_cast<std::uint8_t>(rhs));
}
constexpr ModifierSet operator&(ModifierSet lhs, ModifierSet rhs) {
    return static_cast<ModifierSet>(static_cast<std::uint8_t>(lhs) &
                                    static_cast<std::uint8_t>(rhs));
}
constexpr bool has_modifier(ModifierSet set, ModifierSet flag) {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(flag)) != 0;
}

// One chord. A chord matches when every bit in `required` is held and no bit in `forbidden` is.
// The two masks are what let the table state the original chain's irregular rules as data instead
// of hiding them in control flow:
//
//   - the clipboard/history chords require Control and forbid Alt and Meta (an Alt chord belongs to
//     the app, not the editor), which is why `Copy` lists both in `forbidden`;
//   - the movement and deletion chords ignore Alt and Meta entirely, so an Alt+Left still moves the
//     caret exactly as before;
//   - Shift is only significant where the command distinguishes it (`Redo` via Ctrl+Shift+Z, the
//     extend arms), and `Redo` via Ctrl+Y additionally forbids Shift so Ctrl+Shift+Y keeps its old
//     meaning of "not a redo".
struct KeyChord {
    KeyCode key = key::kUnknown;
    ModifierSet required = ModifierSet::None;
    ModifierSet forbidden = ModifierSet::None;
    EditCommand command = EditCommand::None;
};

// The chord set. FIRST MATCH WINS, so a row that requires more modifiers must precede a row for the
// same key that requires fewer (Ctrl+Shift+Z before Ctrl+Z, Ctrl+Backspace before Backspace).
inline constexpr std::array<KeyChord, 28> kEditingChords = {{
    // Clipboard and history. Alt/Meta are forbidden: those chords are the app's.
    {.key = 'C',
     .required = ModifierSet::Control,
     .forbidden = ModifierSet::Alt | ModifierSet::Meta,
     .command = EditCommand::Copy},
    {.key = 'X',
     .required = ModifierSet::Control,
     .forbidden = ModifierSet::Alt | ModifierSet::Meta,
     .command = EditCommand::Cut},
    {.key = 'V',
     .required = ModifierSet::Control,
     .forbidden = ModifierSet::Alt | ModifierSet::Meta,
     .command = EditCommand::Paste},
    {.key = 'Z',
     .required = ModifierSet::Control | ModifierSet::Shift,
     .forbidden = ModifierSet::Alt | ModifierSet::Meta,
     .command = EditCommand::Redo},
    {.key = 'Z',
     .required = ModifierSet::Control,
     .forbidden = ModifierSet::Alt | ModifierSet::Meta,
     .command = EditCommand::Undo},
    {.key = 'Y',
     .required = ModifierSet::Control,
     .forbidden = ModifierSet::Alt | ModifierSet::Meta | ModifierSet::Shift,
     .command = EditCommand::Redo},
    // Deletion. Shift is not significant (word delete comes from Control alone).
    {.key = key::kBackspace,
     .required = ModifierSet::Control,
     .command = EditCommand::DeleteBackwardWord},
    {.key = key::kBackspace, .command = EditCommand::DeleteBackward},
    {.key = key::kDelete,
     .required = ModifierSet::Control,
     .command = EditCommand::DeleteForwardWord},
    {.key = key::kDelete, .command = EditCommand::DeleteForward},
    // Caret movement: word (Control), extend (Shift), word-extend (Control+Shift), then plain.
    {.key = key::kArrowLeft,
     .required = ModifierSet::Control | ModifierSet::Shift,
     .command = EditCommand::MoveLeftWordExtend},
    {.key = key::kArrowLeft,
     .required = ModifierSet::Control,
     .command = EditCommand::MoveLeftWord},
    {.key = key::kArrowLeft,
     .required = ModifierSet::Shift,
     .command = EditCommand::MoveLeftExtend},
    {.key = key::kArrowLeft, .command = EditCommand::MoveLeft},
    {.key = key::kArrowRight,
     .required = ModifierSet::Control | ModifierSet::Shift,
     .command = EditCommand::MoveRightWordExtend},
    {.key = key::kArrowRight,
     .required = ModifierSet::Control,
     .command = EditCommand::MoveRightWord},
    {.key = key::kArrowRight,
     .required = ModifierSet::Shift,
     .command = EditCommand::MoveRightExtend},
    {.key = key::kArrowRight, .command = EditCommand::MoveRight},
    {.key = key::kHome,
     .required = ModifierSet::Control | ModifierSet::Shift,
     .command = EditCommand::MoveHomeDocumentExtend},
    {.key = key::kHome, .required = ModifierSet::Control, .command = EditCommand::MoveHomeDocument},
    {.key = key::kHome, .required = ModifierSet::Shift, .command = EditCommand::MoveHomeExtend},
    {.key = key::kHome, .command = EditCommand::MoveHome},
    {.key = key::kEnd,
     .required = ModifierSet::Control | ModifierSet::Shift,
     .command = EditCommand::MoveEndDocumentExtend},
    {.key = key::kEnd, .required = ModifierSet::Control, .command = EditCommand::MoveEndDocument},
    {.key = key::kEnd, .required = ModifierSet::Shift, .command = EditCommand::MoveEndExtend},
    {.key = key::kEnd, .command = EditCommand::MoveEnd},
    // Selection. Shift is not significant: Ctrl+A and Ctrl+Shift+A both select all, as before.
    {.key = 'A', .required = ModifierSet::Control, .command = EditCommand::SelectAll},
    // A newline is a multiline-only command the dispatcher gates on the field's `multiline` flag;
    // a single-line field ignores it (submit is a form concern and deferred).
    {.key = key::kEnter, .command = EditCommand::InsertNewline},
}};

// The command a chord maps to, or `EditCommand::None`. First match wins.
[[nodiscard]] constexpr EditCommand command_for(KeyCode key, ModifierSet modifiers) noexcept {
    for (const KeyChord& chord : kEditingChords) {
        if (chord.key != key) {
            continue;
        }
        if ((modifiers & chord.required) != chord.required) {
            continue;
        }
        if ((modifiers & chord.forbidden) != ModifierSet::None) {
            continue;
        }
        return chord.command;
    }
    return EditCommand::None;
}

} // namespace tessera::ui::text
