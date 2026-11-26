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

#include <optional>
#include <string>
#include <vector>

namespace tessera::compiler::codegen::cli {

class PreviewSession;

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

} // namespace tessera::compiler::codegen::cli
