// SPDX-License-Identifier: MIT

#pragma once

// The preview session's per-binding records, shared by the two translation units that implement the
// session's event path: `scene_session_events.cpp` (the scene-event bindings) and
// `scene_session_size_change.cpp` (`onSizeChange`). Not installed; only tools/codegen includes it.
//
// Both are `unique_ptr`-owned by the session because `Node::bind_event` and `LayoutObservation`
// hold a `void*` context for the binding's whole life, so the record must never move.

#include "scene_build.hpp"
#include "tessera/ui/layout/layout_observer.hpp"
#include "tessera/ui/text/text_input_runtime.hpp"

#include <optional>
#include <string>
#include <vector>

namespace tessera::compiler::codegen::cli {

class PreviewSession;

// Fills the text payload and event type of `input` (a Text or Composition kind) into `event`. A
// commit is an `EditText`, which the controller treats as one insert; a payload past the event's
// bounded buffer is cut at a code point boundary. Defined in `scene_session_text.cpp`.
void fill_text_event(const PreviewInput& input, tessera::ui::InputEvent& event);

// The IR body of `handler_id` in `component` (a component handler or a native binding's preview
// block); false when neither exists. Defined in `scene_session_events.cpp`.
bool resolve_body(const ir::IrModule& module, const ir::Component& component,
                  const std::string& handler_id, const std::vector<ir::Param>*& params,
                  const std::vector<ir::Action>*& actions);

// One installed scene binding. `bind_event` takes a function pointer plus a `void*`, so the IR
// handler identity has to live in the context; this record is that context.
struct PreviewSession::HandlerSlot {
    std::string handler_id;
    std::size_t frame = 0; // the component frame the handler belongs to (0 is the root)
    const std::vector<ir::Param>* params = nullptr;
    const std::vector<ir::Action>* actions = nullptr;
    bool can_run = false;
    // Set by the scene callback: this dispatch reached the handler. Consumed (and cleared) by
    // `dispatch()`, which runs the body after the router has finished walking the tree.
    bool reached = false;
    std::string reason; // why not, when `can_run` is false
};

// One installed `onSizeChange` observation (ADR-041 item 3, issue #694). The preview runs the IR
// body the generated base's thunk would call, with a `SizeChangeEvent` record bound to the
// handler's parameter.
struct PreviewSession::SizeSlot {
    PreviewSession* session = nullptr;
    std::string handler_id;
    const std::vector<ir::Param>* params = nullptr;
    const std::vector<ir::Action>* actions = nullptr;
    bool can_run = false;
    std::string reason; // why not, when `can_run` is false
    // The observation itself. Declared last so the context pointer (`this`) is stable.
    std::optional<tessera::ui::layout::LayoutObservation> observation;
};

// One live `TextInput`/`TextArea` (issue #601): the engine's own edit controller and runtime, plus
// the `onChange` body. It outlives a rebuild (keyed by automation id) so the caret, the selection,
// the undo history and an IME composition survive the rebuild an `onChange` triggers. The context
// pointers handed to `Node::bind_event` are this record's address, so it never moves.
struct PreviewSession::TextField {
    TextField(PreviewSession* owner, std::uint64_t id)
        : session(owner),
          automation_id(id),
          runtime(editor) {}
    PreviewSession* session = nullptr;
    std::uint64_t automation_id = 0;
    tessera::ui::text::EditController editor;
    tessera::ui::text::TextInputRuntime runtime;
    std::string handler_id; // the `onChange` handler, empty when the field has none
    std::size_t frame = 0;
    const std::vector<ir::Param>* params = nullptr;
    const std::vector<ir::Action>* actions = nullptr;
    bool can_run = false;
};

} // namespace tessera::compiler::codegen::cli
