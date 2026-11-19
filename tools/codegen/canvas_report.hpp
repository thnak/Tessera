// SPDX-License-Identifier: MIT

#pragma once

#include "scene_canvas_patch.hpp"
#include "scene_canvas_session.hpp"
#include "scene_frame.hpp"
#include "scene_state.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/ui/scene/state_snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The reports the canvas module returns to the page (issue #167, P3 of
// docs/design/reference-equantic-ui.md section 7.4; the pointer report is issue #388).
//
// They are plain C++ functions in their own translation unit, not lambdas inside the Emscripten
// entry, so that the host lanes compile them too: `wasm_canvas_entry.cpp` includes the WebGPU
// backend header and can only be built by Emscripten, and the serializer is the part of that entry
// which can still be type-checked (and tested) on a normal Windows/Linux build. Same shape the
// P1/P2 modules use, adapted to an entry the host cannot include.
namespace tessera::compiler::codegen::cli {

// One JSON object, one line, with the field names the native CLI prints. `has_device` distinguishes
// "the page never handed over a WebGPU device" from "the frame itself failed", which are different
// things for a page to report: the first is an unsupported browser, the second is a bug.
//
// `frames` is `CanvasSceneSession::frames_painted()` for the report that describes a session paint
// (#444), and 0 for the stateless `tessera_canvas_frame` path, which has no session to count.
//
// `css_diagnostics` is the author stylesheet's parse problems (#390), appended as
//
//   "cssDiagnostics":[{"code":"TESSERA_CSS_UNKNOWN_PROPERTY","severity":"warning",
//                      "message":"unsupported property","file":"ui/Preview.css",
//                      "line":3,"column":5,"offset":42},...]
//
// The field is APPENDED, so `canvasVersion` stays 1 (docs/specs/versioning-v1.md section 2: adding
// an optional member keeps the version, and a consumer tolerates an unknown field it was told
// about). That matters because the frame's own fields did not change: a page built against the
// previous module reads the same report, and an old page simply never looks at the new array. The
// array is capped at `kMaxReportedCssDiagnostics` entries, and `cssDiagnosticsTruncated` says when
// the cap was hit, so "16 problems" is never mistaken for "all of them".
//
// Both parameters are defaulted so the two features' callers keep their shapes: a pre-#444 caller
// writes `canvas_report_json(result, w, h, has_device)` and a #390-only caller may pass just the
// diagnostics. There is ONE report document, not two -- the merge appends both to the same
// serializer rather than making a page pick a shape, which is what lets a session paint report its
// frame count and its stylesheet diagnostics together.
//
// `cascade_generation` is `CanvasSceneSession::cascade_generation()` (issue #436): how many times
// the running evaluation's cascade has been resolved. It is in the frame report because "a resize
// that crosses no `@media` breakpoint does not rebuild the cascade" is otherwise invisible from the
// page, and a developer watching the Canvas pane - or the browser probe - can read the counter move
// by one per crossed breakpoint instead of inferring it from a picture.
[[nodiscard]] std::string
canvas_report_json(const SceneFrameResult& result, std::uint32_t width, std::uint32_t height,
                   bool has_device, std::uint32_t frames = 0,
                   const std::vector<PreviewCssDiagnostic>& css_diagnostics = {},
                   std::uint64_t cascade_generation = 0);

// What one pointer event did, as one JSON object on one line:
//
//   {"pointerVersion":1,"dispatched":true,"changed":true,"targetId":"…","hoveredId":"…",
//    "activeId":"…","ran":["handler::1"],"unrun":[],"error":""}
//
// `changed` is the page's whole decision: it schedules **one** frame when it is true and none when
// it is false, which is how a static scene presents nothing. Ids are decimal strings because every
// id in this project's documents is one (automation-ids-v1 section 1) and a `uint64` uiId does not
// survive `JSON.parse` as a number.
[[nodiscard]] std::string canvas_pointer_report_json(const CanvasPointerResult& result);

// What the picker found under one point (issue #171's picker slice), as one JSON object on one
// line:
//
//   {"pickVersion":1,"found":true,"automationId":"101","elementKind":"Button",
//    "box":{"x":12,"y":40,"width":120,"height":32}}
//
// `found:false` is a real answer, not an error: the pointer is over no node (outside the viewport,
// or over a gap the tree does not cover), and the page hides the overlay. The box is in **logical
// viewport pixels** - the space `tessera_canvas_pointer` takes its coordinates in - so the page
// scales it by the CSS-to-logical ratio it already applies to pointer input, never by the DPR.
//
// `elementKind` is the node's element kind (`Button`, `Text`, `View`, ...), which is what the
// readout shows beside the id: an id alone does not tell a developer what they are looking at. It
// is borrowed from the running evaluation, so it is valid until the next apply - which is exactly
// the lifetime of the report the page reads it from.
[[nodiscard]] std::string canvas_pick_report_json(const CanvasPickResult& result);

// What the style inspector found for one node (issue #171, designer-protocol-v1 section 9.1), as
// one JSON object on one line:
//
//   {"styleVersion":1,"ok":true,"node":"101",
//    "computed":{"background-color":"#4a86ff","color":"#ffffff"},
//    "matched":[{"origin":"author","selector":"Button","specificity":[0,0,1],
//                "span":{"file":"ui/Preview.css","offset":120,"length":0},
//                "declarations":[{"property":"background-color","value":"#4a86ff",
//                                 "important":false,"overridden":false}]}],
//    "box":{"x":12,"y":40,"width":120,"height":32,
//           "margin":[0,0,0,0],"border":[0,0,0,0],"padding":[8,8,8,8]},
//    "overrides":[]}
//
// A missing node is `{"styleVersion":1,"ok":false,"reason":"no_such_node"}` and **no other field**
// (designer-protocol-v1 section 9.1): a page that read a stale `computed` off a failed answer would
// show the previous node's style under the new node's name.
//
// `computed` is an object rather than an array because the spec says "property name -> resolved
// value string"; a JSON object is the only shape that says that, and the page reads it as a map.
// `matched` is highest precedence first, which is the engine's own order, not a sort the page
// applies. `overridden` is the cascade's answer, so a page never has to compare specificities.
//
// `span` is the SourceSpan of the rule (designer-protocol-v1 section 3.1) with `length` 0: the
// engine knows where a rule starts, not how long its selector is, and a zero-length span at the
// rule's offset is what an editor needs to put a cursor there. `file` is empty for a rule resolved
// from a blob, which has no source file to point into.
[[nodiscard]] std::string canvas_style_report_json(const StyleInfoResult& result);

// What one simulated fold-posture change did (issue #408), as one JSON object on one line:
//
//   {"foldVersion":1,"changed":true,"error":""}
//
// `changed` is `PreviewSession::set_posture()`'s answer: true when a layout pass ran, false when
// the posture was already the one in force (so the page draws no frame). The layout pass completes
// inside the call, so a page that draws once on `changed` paints the new posture on its first
// frame, which is the "no frame of stale padding" rule this serializer exists to make observable.
[[nodiscard]] std::string canvas_fold_report_json(const SceneFrameResult& result, bool changed);

// What one viewport change did (issue #436), as one JSON object on one line:
//
//   {"viewportVersion":1,"changed":true,"width":390,"height":844,"dpr":1,"cascadeGeneration":2,
//    "error":""}
//
// `changed` is `CanvasSceneSession::set_viewport()`'s answer: true when a layout pass ran, false
// when the rectangle was already in force (so the page draws no extra frame). `cascadeGeneration`
// is the session's own count of cascade resolutions, and it is the field that makes the breakpoint
// rule observable from outside: a size that crosses no `@media` breakpoint lays out without moving
// the counter, and one that crosses a breakpoint moves it by exactly one. `width`/`height`/`dpr`
// are what the session was asked for, so a page can report the device it actually got rather than
// the one it requested.
[[nodiscard]] std::string canvas_viewport_report_json(bool changed, float width, float height,
                                                      float dpi, std::uint64_t cascade_generation,
                                                      std::string_view error);

// What one *apply* did (issue #444: the editor's live edit), as one JSON object on one line:
//
//   {"applyVersion":1,"changed":true,"reused":false,"carried":1,"droppedComponent":0,
//    "droppedCell":0,"droppedType":0,"added":0,"frames":3,"error":""}
//
// `changed` is `CanvasSceneSession::needs_frame()` after the apply, and it is the page's whole
// decision: true means draw exactly one frame, false means the picture on the canvas is already
// the one this source describes -- which is how an auto-apply of unchanged text costs no frame.
//
// `frames` is `CanvasSceneSession::frames_painted()`, the session's own lifetime count. It is in
// this report because the live edit's contract is a *frame count* ("one frame per burst", "zero
// frames when static") and a count the page reads is evidence, where a count the engine asserts in
// a unit test is only a promise. The page surfaces it in the Canvas pane's counters, so the browser
// probe can check the rule against the same number the engine used.
//
// `error` non-empty means the source did not evaluate and **the running evaluation was kept**:
// `changed` is then whatever it already was, and the page shows the error as a diagnostic while the
// last good frame stays on screen. That distinction is why this report exists at all: a page cannot
// tell "the edit applied, draw" from "the edit failed, keep what you have" by looking at a frame
// counter.
//
// `carried`/`dropped*` are `StateCarryReport`'s split, so the page can say which cells survived an
// edit and why the others did not (`scene_state.hpp` is the rule). A non-zero `droppedComponent` is
// the structural-change case the spec calls a reset: the node a cell lived on is gone.
//
// `state` is the running evaluation's state cells **after** the apply, in declaration order:
//
//   [{"name":"state::count","value":2}]
//
// It is here because "a state cell survives a text edit" is an acceptance sentence of the live edit
// and the canvas pane is where a developer watches it. Before #444 the canvas had no way to show
// its own state at all -- the Scene pane's input script was the only place a cell was ever printed
// -- so a developer changing a string could not tell a carried count from a reset one. `value` is a
// JSON scalar or string; a list cell is reported as its JSON array, which is the same shape
// `snapshot_state()` uses.
[[nodiscard]] std::string canvas_apply_report_json(bool changed, bool reused,
                                                   const StateCarryReport& carry,
                                                   const ui::StateSnapshot& state,
                                                   std::uint32_t frames, std::string_view error,
                                                   const std::vector<std::string>& components = {},
                                                   std::string_view component = {});

// What one style commit produced (issue #171, designer-protocol-v1 section 10.2), as one JSON
// object on one line:
//
//   {"patchVersion":1,"ok":true,"patchId":"...","edits":[{"file":"ui/Card.tsx","offset":42,
//    "length":24,"newText":"style={{ ... }}"}],"baseVersions":{"ui/Card.tsx":1}}
//
// or, when the commit cannot be turned into edits:
//
//   {"patchVersion":1,"ok":false,"reason":"not_token_bound"}
//
// **`ok:false` carries no `edits` and no `baseVersions`**, because the spec sends no `source.patch`
// for a failed commit (section 10.2). A page that read an empty edit list off a failure would show
// "0 edits" where the truth is "nothing was produced".
//
// `edits` is in the order the spec requires - sorted by file and descending offset - so a host
// applies them in the order given and never shifts a later edit. `baseVersions` is an object rather
// than an array because the spec calls it a map from file path to version, and the page compares it
// with its own copies by key.
//
// **`patchId` is the viewer's own handle for the patch**, which the spec's `style.commit.result`
// carries so the following `source.patch` can name it (section 10.2). It is derived from the node
// and the destination, so two commits of the same node to the same destination in one session get
// the same id and a host can tell a retry from a new commit.
[[nodiscard]] std::string canvas_patch_report_json(const SourcePatchResult& result);

// What one override operation did (issue #536, designer-protocol-v1 sections 9.2-9.4), one line:
//
//   {"overrideVersion":1,"ok":true,"changed":true,"canUndo":true,"canRedo":false,
//    "overrides":[{"node":"123","property":"color","value":"red"}]}
//
// or, for a refused declaration or a missing node:
//
//   {"overrideVersion":1,"ok":false,"reason":"invalid_override","changed":false,"canUndo":...,
//    "diagnostics":[{"code":"TESSERA_CSS_INVALID_VALUE","message":"..."}],"overrides":[...]}
//
// `node` is a decimal string, like every id in this module, because a 64-bit `uiId` does not
// survive `JSON.parse` as a number. `diagnostics` is present only when there is one.
[[nodiscard]] std::string canvas_override_report_json(const OverrideOutcome& outcome);

// The answer to settling a commit (section 10.3): `{"settleVersion":1,"ok":true,"consumed":true,
// "overrides":[]}`, or `ok:false` with `reason` (`no_such_node` or `patch_mismatch`) and, for a
// mismatch, a `TESSERA_DESIGNER_PATCH_MISMATCH` diagnostic the page forwards as a `diagnostic`.
[[nodiscard]] std::string canvas_settle_report_json(const CommitSettleResult& result);

} // namespace tessera::compiler::codegen::cli
