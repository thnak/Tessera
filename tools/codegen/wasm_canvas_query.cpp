// SPDX-License-Identifier: MIT

// The canvas module's *query* exports (issue #171): the picker, the style inspector and the
// commit's patch. They read the running session and answer a value; none of them dispatches an
// event, marks anything dirty or owes a frame.
//
// **Why they are their own translation unit.** `wasm_canvas_entry.cpp` owns the module's one device
// and one session, so the exports that *change* that state have to stay together. These three do
// not: they take the session through `canvas_query_session()` and never create, replace or reset
// it. Splitting them out is what keeps the entry inside the source-size limit, and it groups the
// three exports a page calls on a pointer move or a selection - the ones that must stay cheap - in
// one place where "this is a query" is visible.
//
// The report serializers are still in `canvas_report.cpp`, so a host lane type-checks them without
// Emscripten; this unit is only the C spelling of the three calls.

#include "canvas_report.hpp"
#include "scene_canvas_session.hpp"
#include "wasm_canvas_request.hpp"

#include <cstdint>
#include <cstdlib>
#include <emscripten/emscripten.h>
#include <string_view>

namespace tessera::compiler::codegen::cli {

// Defined in `wasm_canvas_entry.cpp`, which owns the module's state.
[[nodiscard]] CanvasSceneSession* canvas_query_session();
[[nodiscard]] char* canvas_duplicate_out(std::string_view text);

} // namespace tessera::compiler::codegen::cli

extern "C" {

// What is under `(x, y)` in logical viewport pixels, for the picker overlay (issue #171).
//
// **Why this is its own export and not a pointer kind.** The picker must be able to ask "what is
// here?" without changing anything: arming it and moving over a button must not set `:hover`, must
// not run a handler and must not cost a frame. A `pointer()` kind would dispatch, and the page
// would then have to undo the hover it never wanted. This reads the tree with the router's own hit
// test (`Node::hit_test_deep`) and answers a value, so the node it names is the node a press at the
// same point would reach - one hit test, not two that can disagree.
//
// It answers a well-formed report in every state: no session, no tree and a point outside every
// node are all `found:false`, so a page that arms the picker before the first Run costs no
// exception at the FFI boundary and shows no stale outline.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_pick(float x, float y) {
    tessera::compiler::codegen::cli::CanvasPickResult result;
    tessera::compiler::codegen::cli::CanvasSceneSession* session =
        tessera::compiler::codegen::cli::canvas_query_session();
    if (session != nullptr && session->started()) {
        result = session->pick(x, y);
    }
    return tessera::compiler::codegen::cli::canvas_duplicate_out(
        tessera::compiler::codegen::cli::canvas_pick_report_json(result));
}

// The style inspector's query (issue #171, designer-protocol-v1 section 9.1): the computed style,
// the matched rules with their provenance, and the box of the node with this automation id.
//
// **Why this is its own export and not part of `tessera_canvas_pick`.** A pick answers "what is
// under this point"; the inspector answers "what styles this node", and the two are asked at
// different moments: the picker runs on every pointer move and must stay cheap, while the inspector
// runs once when a node is selected and may allocate. Keeping them apart is also what lets the
// picker stay a two-string query.
//
// **It is a query.** Nothing is dispatched, no pseudo-class moves and no frame is owed: opening the
// inspector on a node must not change the picture. The id is a decimal string because a 64-bit
// `uiId` does not survive `JSON.parse` as a JS number, the same convention every other id in this
// module follows.
//
// A missing node, an id of 0 and a session with no tree all answer
// `{"styleVersion":1,"ok":false,"reason":"no_such_node"}` with no other field, so a page that asks
// before the first Run costs no exception at the FFI boundary and shows no stale style.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_style(const char* automation_id) {
    using namespace tessera::compiler::codegen::cli;
    StyleInfoResult result;
    CanvasSceneSession* session = canvas_query_session();
    if (session != nullptr && session->started()) {
        // `strtoull` rather than `std::stoull`: the latter throws on a malformed id, and nothing
        // may throw across the FFI boundary. A malformed id parses to 0, which is `no_such_node`.
        const char* text = automation_id != nullptr ? automation_id : "";
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(text, &end, 10);
        if (end != text && *end == '\0') {
            result = session->inspect(static_cast<std::uint64_t>(parsed));
        }
    }
    return canvas_duplicate_out(canvas_style_report_json(result));
}

// The style commit's patch (issue #171, designer-protocol-v1 section 10.2): the minimal edit list a
// host applies, or the spec's reason there is none.
//
//   automation_id  the node's automation id, decimal (the same id `tessera_canvas_style` takes)
//   destination    "inline" | "class" | "token"
//   class_name     the `class` destination's class, without the leading `.`; empty otherwise
//   file           the `class` destination's CSS file path; empty otherwise
//   token          the `token` destination's custom property, WITH its `--` prefix; empty otherwise
//   token_value    the value the token declaration takes; empty otherwise
//
// There is no declarations argument: a commit writes ALL of the node's overrides from the session's
// override layer (section 10.1, issue #536), so the list is never the page's to supply. A node with
// no overrides answers `no_overrides`.
//
// **It is a query, and that is the point.** Nothing is written, no file is touched and no frame is
// owed: the viewer produces a value the host applies and answers with `source.patch.result`
// (section 10.3). A page that shows the edits has not changed the project, and this export is what
// makes that distinction structural rather than a promise the page has to keep.
//
// A missing session, an id of 0 and a malformed id all answer
// `{"patchVersion":1,"ok":false,"reason":"no_such_node","detail":"..."}`, so a page that asks
// before the first Run costs no exception at the FFI boundary.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_patch(const char* automation_id,
                                                      const char* destination,
                                                      const char* class_name, const char* file,
                                                      const char* token, const char* token_value) {
    using namespace tessera::compiler::codegen::cli;
    // The argument parsing is `canvas_patch_request()` (`wasm_canvas_request.cpp`), the same split
    // every other export makes: the rules a page can get wrong live in a unit a host lane can
    // type-check, and this export stays a call and a report.
    const PatchRequest request = canvas_patch_request(
        automation_id != nullptr ? automation_id : "", destination != nullptr ? destination : "",
        class_name != nullptr ? class_name : "", file != nullptr ? file : "",
        token != nullptr ? token : "", token_value != nullptr ? token_value : "");
    SourcePatchResult result;
    CanvasSceneSession* session = canvas_query_session();
    if (session != nullptr && session->started()) {
        result = session->patch(request);
    } else {
        result.reason = PatchErrorCode::NoSuchNode;
        result.detail = "no evaluation is running";
    }
    return canvas_duplicate_out(canvas_patch_report_json(result));
}

} // extern "C"
