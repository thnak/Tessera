// SPDX-License-Identifier: MIT
//
// Scripted edit sequence of the generated-UI demo: input events go through the scene router so the
// generated source-event bindings drive the edit controller. Split from generated_ui_tests.cpp.

#pragma once

#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/edit_controller.hpp"

#include <cstdint>
#include <string_view>

namespace generated_ui_field_script {

using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Modifier;
using tessera::ui::Node;
using tessera::ui::text::EditController;

// --- Editable text field script ----------------------------------------------------------------
//
// The edit controller is the owner of text/caret/selection; the demo publishes its state into the
// generated TextInput node as PaintData text + decoration (the controlled-value gap: the generated
// component never feeds the painted field value itself yet). The script covers typing, caret moves,
// Shift-selection, select-all and delete-selection, and returns with the editable frame:
// "Ada Lovelace" with "Lovelace" selected (caret at the end of the selection).

// Feeds normalized input events through the scene router so the generated source-event bindings and
// typed TextChange adapter drive the edit controller (no direct controller calls).
inline bool feed_text(InputRouter& router, Node& root, std::string_view text) {
    for (const char character : text) {
        InputEvent event;
        event.type = InputEventType::Text;
        event.text_codepoint = static_cast<std::uint32_t>(static_cast<unsigned char>(character));
        if (!router.dispatch(root, event).dispatched) {
            return false;
        }
    }
    return true;
}

inline bool feed_key(InputRouter& router, Node& root, std::uint32_t key,
                     Modifier modifiers = Modifier::None) {
    InputEvent event;
    event.type = InputEventType::KeyDown;
    event.key = key;
    event.modifiers = modifiers;
    return router.dispatch(root, event).dispatched;
}

inline bool run_field_script(InputRouter& router, Node& root, EditController& editor) {
    using namespace tessera::ui::key;
    // Type, then exercise Home/End/arrows.
    if (!feed_text(router, root, "Ada") || editor.buffer().text() != "Ada" ||
        editor.buffer().caret() != 3) {
        return false;
    }
    if (!feed_key(router, root, kHome) || editor.buffer().caret() != 0) {
        return false;
    }
    if (!feed_key(router, root, kEnd) || editor.buffer().caret() != 3) {
        return false;
    }
    if (!feed_key(router, root, kArrowLeft) || editor.buffer().caret() != 2) {
        return false;
    }
    // Shift+Left twice: half-open selection [0, 2) = "Ad", caret at 0 (lean left).
    if (!feed_key(router, root, kArrowLeft, Modifier::Shift) ||
        !feed_key(router, root, kArrowLeft, Modifier::Shift) || editor.buffer().caret() != 0 ||
        editor.buffer().selection_start() != 0 || editor.buffer().selection_end() != 2) {
        return false;
    }
    // Select-all then delete the selection: the buffer empties and the caret collapses to 0.
    if (!feed_key(router, root, 'A', Modifier::Control) || !editor.buffer().has_selection()) {
        return false;
    }
    if (!feed_key(router, root, kBackspace) || !editor.buffer().text().empty() ||
        editor.buffer().has_selection() || editor.buffer().caret() != 0) {
        return false;
    }
    // Editable frame: type a full value, put the caret after "Ada ", then Shift+End to select
    // "Lovelace" with the caret at the right edge.
    if (!feed_text(router, root, "Ada Lovelace") || editor.buffer().text() != "Ada Lovelace") {
        return false;
    }
    if (!feed_key(router, root, kHome) || editor.buffer().caret() != 0) {
        return false;
    }
    for (int step = 0; step < 4; ++step) {
        if (!feed_key(router, root, kArrowRight)) {
            return false;
        }
    }
    if (editor.buffer().caret() != 4 || !feed_key(router, root, kEnd, Modifier::Shift)) {
        return false;
    }
    return editor.buffer().caret() == 12 && editor.buffer().selection_start() == 4 &&
           editor.buffer().selection_end() == 12;
}

} // namespace generated_ui_field_script
