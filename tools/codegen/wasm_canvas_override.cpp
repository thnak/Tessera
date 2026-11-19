// SPDX-License-Identifier: MIT

// The canvas module's override exports (issue #536, designer-protocol-v1 sections 9.2-9.4 and
// 10.3): `style.override.set/clear/revert`, `edit.undo`/`edit.redo`, and the settle that closes a
// commit's round trip.
//
// **Why they are not in `wasm_canvas_query.cpp`.** Those exports are queries: nothing changes and
// no frame is owed. These change the layer and restyle the running tree, so a frame IS owed
// afterwards (each report's `changed` says so, and `needs_frame()` agrees). They still take the
// session through `canvas_query_session()` and never create, replace or reset it, which is what
// keeps the module's one owner of the session in `wasm_canvas_entry.cpp`.

#include "canvas_report.hpp"
#include "scene_canvas_session.hpp"

#include <cstdint>
#include <cstdlib>
#include <emscripten/emscripten.h>
#include <string>
#include <string_view>

namespace tessera::compiler::codegen::cli {

// Defined in `wasm_canvas_entry.cpp`, which owns the module's state.
[[nodiscard]] CanvasSceneSession* canvas_query_session();
[[nodiscard]] char* canvas_duplicate_out(std::string_view text);

namespace {

// `strtoull` rather than `std::stoull`: the latter throws on a malformed id, and nothing may throw
// across the FFI boundary. A malformed id parses to 0, which names no node.
std::uint64_t parse_id(const char* text) {
    const char* const source = text != nullptr ? text : "";
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(source, &end, 10);
    return end != source && *end == '\0' ? static_cast<std::uint64_t>(parsed) : 0;
}

} // namespace
} // namespace tessera::compiler::codegen::cli

extern "C" {

// One override operation. `operation` is `set`, `clear`, `revert`, `undo` or `redo`; the other
// arguments are the operation's and unused ones are empty:
//
//   set     automation_id, property, value
//   clear   automation_id, and property (empty clears every override of the node)
//   revert, undo, redo   no arguments
//
// Answers `canvas_override_report_json`: `changed` says a frame is owed, `overrides` is the layer
// afterwards, and a refused declaration carries its `TESSERA_CSS_*` diagnostic with the layer
// unchanged. An unknown operation, or a session with no tree, answers `ok:false` rather than
// throwing across the boundary.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_override(const char* operation,
                                                         const char* automation_id,
                                                         const char* property, const char* value) {
    using namespace tessera::compiler::codegen::cli;
    OverrideOutcome outcome;
    CanvasSceneSession* session = canvas_query_session();
    const std::string_view op = operation != nullptr ? operation : "";
    const std::string_view property_text = property != nullptr ? property : "";
    const std::string_view value_text = value != nullptr ? value : "";
    if (session == nullptr || !session->started()) {
        outcome.reason = "no_such_node";
    } else if (op == "set") {
        outcome = session->override_set(parse_id(automation_id), property_text, value_text);
    } else if (op == "clear") {
        outcome = session->override_clear(parse_id(automation_id), property_text);
    } else if (op == "revert") {
        outcome = session->override_revert();
    } else if (op == "undo") {
        outcome = session->edit_undo();
    } else if (op == "redo") {
        outcome = session->edit_redo();
    } else {
        outcome.reason = "unknown_operation";
    }
    return canvas_duplicate_out(canvas_override_report_json(outcome));
}

// Closes a commit's round trip (section 10.3): call it after `source.patch.result` said `applied`
// and the following `source.update` has been evaluated. The node's overrides leave the layer when
// the new evaluation gives the node what they produced; otherwise they stay and the report carries
// a `TESSERA_DESIGNER_PATCH_MISMATCH` diagnostic for the page to forward.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_settle(const char* automation_id) {
    using namespace tessera::compiler::codegen::cli;
    CommitSettleResult result;
    CanvasSceneSession* session = canvas_query_session();
    if (session != nullptr && session->started()) {
        result = session->settle_commit(parse_id(automation_id));
    } else {
        result.reason = "no_such_node";
    }
    return canvas_duplicate_out(canvas_settle_report_json(result));
}

// Hands the project's theme asset to the session so the `token` commit destination has a file to
// edit (issue #536; until it is called `tessera_canvas_patch` answers `not_token_bound`).
//
//   file      the theme asset's project path (`tessera.json` `assets.theme`)
//   text      the asset's bytes as the editor holds them: a patch's offsets are measured in
//             THESE bytes, so a stale buffer gets edits for a file that has moved
//   theme_id  the theme whose token object a commit may write
//
// Call it after `tessera_canvas_apply`: the first apply creates the session and
// `tessera_canvas_reset` drops it, and the handed-over asset lives and dies with it. Empty `text`
// clears the hand-over (a project with no theme asset). It never creates a session and owes no
// frame; it answers `{"ok":true}` when a session took it and `{"ok":false}` when none exists.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_set_theme(const char* file, const char* text,
                                                          const char* theme_id) {
    using namespace tessera::compiler::codegen::cli;
    CanvasSceneSession* session = canvas_query_session();
    if (session == nullptr) {
        return canvas_duplicate_out("{\"ok\":false}");
    }
    session->set_theme_asset(file != nullptr ? file : "", text != nullptr ? text : "",
                             theme_id != nullptr ? theme_id : "");
    return canvas_duplicate_out("{\"ok\":true}");
}

} // extern "C"
