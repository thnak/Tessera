// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/text/edit_controller.hpp"

#include <cstddef>
#include <cstdint>

namespace tessera::ui::text {

// Runtime half of the controlled-text-input contract (docs/specs/text-editing-v1.md section 12):
// wires one `EditController` to the scene node that owns it and turns every committed edit into
// exactly one `EventType::TextChange` scene event.
//
// Wiring (the owner is the app component or custom element that owns the field):
//
//     editor.attach(node);                       // node must outlive the runtime
//     // in the node's on_event, before/after any other handling:
//     if (runtime.handle_event(event)) { event.handled = true; }
//
// The change event is dispatched synchronously through the normal capture -> target -> bubble path
// with `dispatch_event_to`, so it reaches the node and its ancestors exactly like any other scene
// event. The controller consumes `Text`/`EditText`/`Composition*`/editing `KeyDown` events only
// while the attached node carries `PseudoState::Focus`; read-only controllers never mutate, so they
// never emit. `prevent_default`/already-`handled` source events are ignored by the controller and
// produce no change event.
//
// Emission rule: one change event per committed text mutation (Text insert, EditText/composition
// commit, Backspace/Delete/paste through the controller). Preedit `CompositionUpdate`s, selection
// or caret moves, the external controlled-value `set_text` sync, and auto-repeat events that do not
// mutate the text emit nothing. A repeated Backspace that really deletes is its own committed edit
// and emits once for that deletion.
//
// This class lives in `tessera_text_edit` rather than `tessera_scene` because the scene layer
// cannot depend on the text layer; it only uses the scene's public dispatch API.
class TextInputRuntime {
public:
    explicit TextInputRuntime(EditController& controller);

    TextInputRuntime(const TextInputRuntime&) = delete;
    TextInputRuntime& operator=(const TextInputRuntime&) = delete;

    // Attaches the scene node that receives the change events. `nullptr` detaches. No ownership:
    // the node must outlive the runtime (detach before destroying the node).
    void attach(Node* node);
    void detach() {
        attach(nullptr);
    }
    [[nodiscard]] Node* target() const {
        return target_;
    }

    // Offers one scene event to the attached controller. Returns true when the controller consumed
    // it (the event is then marked `handled`). A committed mutation emits one `TextChange` event
    // before returning.
    bool handle_event(Event& event);

    // Number of `TextChange` events emitted since construction (diagnostics/tests).
    [[nodiscard]] std::size_t change_events() const {
        return change_events_;
    }
    // `handled` flag of the most recently dispatched change event. The runtime marks it when the
    // target has a `TextChange` binding; an `on_event` handler may also set it.
    [[nodiscard]] bool last_change_handled() const {
        return last_change_handled_;
    }

private:
    void emit_change();

    EditController* controller_ = nullptr;
    Node* target_ = nullptr;
    std::uint64_t seen_committed_edits_ = 0;
    std::size_t change_events_ = 0;
    bool last_change_handled_ = false;
};

} // namespace tessera::ui::text
