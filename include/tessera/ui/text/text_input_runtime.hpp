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
    // Const overload for node event bindings (which receive `const Event&`): works on a copy, so
    // the source event's `handled` flag stays advisory. Generated code that does not override
    // `Node::on_event` uses `bind_text_input_events` below instead of this call directly.
    bool handle_event(const Event& event);

    // Publishes the controller's caret/selection/preedit state into the attached node's
    // `PaintData::decoration` (Text-kind payload only). Generated controlled fields call this at
    // the end of `apply_state` because that rebuilds the PaintData, and the runtime calls it after
    // every handled event, so paint sees the caret without a timer. Caret visibility follows focus
    // (`EditController::caret_blink_active`); `blink_phase` keeps the paint default until a host
    // wires `CaretBlink`. No-op without a target or a Text-kind payload.
    void publish_decoration();

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

// Trampoline for a node event binding: forwards the bound scene event to the runtime. Declared
// outside the class so generated code can register it without a lambda.
inline void text_input_event_trampoline(void* context, const Event& event) {
    static_cast<TextInputRuntime*>(context)->handle_event(event);
}

// Binds the six source event types a text field consumes (`Text`, `EditText`, the composition
// family and `KeyDown`) to `runtime` on `node`. This is the wiring path for generated components,
// which build plain `Node` trees and register event bindings instead of overriding
// `Node::on_event`. It consumes six of the node's eight binding slots; bind the `TextChange`
// handler separately (one more slot, seven total). Returns false when a binding is rejected (null
// node/handler or the slot budget is exhausted).
template<typename NodeT> bool bind_text_input_events(NodeT& node, TextInputRuntime& runtime) {
    constexpr EventType kTypes[] = {EventType::Text,
                                    EventType::EditText,
                                    EventType::CompositionStart,
                                    EventType::CompositionUpdate,
                                    EventType::CompositionEnd,
                                    EventType::KeyDown};
    for (const EventType type : kTypes) {
        if (!node.bind_event(type, &text_input_event_trampoline, &runtime)) {
            return false;
        }
    }
    return true;
}

} // namespace tessera::ui::text
