// SPDX-License-Identifier: MIT

#pragma once

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
// `frames` is `CanvasSceneSession::frames_painted()` for the report that describes a session paint,
// and 0 for the stateless `tessera_canvas_frame` path, which has no session to count. It is a default
// argument so the error-path reports (which never painted) do not have to spell it out.
[[nodiscard]] std::string canvas_report_json(const SceneFrameResult& result, std::uint32_t width,
                                             std::uint32_t height, bool has_device,
                                             std::uint32_t frames = 0);

// The same report with the author stylesheet's parse problems appended (issue #390), as
//
//   "cssDiagnostics":[{"code":"TESSERA_CSS_UNKNOWN_PROPERTY","severity":"warning",
//                      "message":"unsupported property","file":"ui/Preview.css",
//                      "line":3,"column":5,"offset":42},...]
//
// `frames` is the same session counter as in the frame-only overload (#444), so a session paint can
// report both its frame count and its stylesheet diagnostics: the two features append to the same
// document and neither is a second report.
//
// The field is APPENDED, so `canvasVersion` stays 1 (docs/specs/versioning-v1.md section 2: adding
// an optional member keeps the version, and a consumer tolerates an unknown field it was told
// about). That matters because the frame's own fields did not change: a page built against the
// previous module reads the same report, and an old page simply never looks at the new array.
//
// The array is capped at `kMaxReportedCssDiagnostics` entries. A bounded report is the point: a
// stylesheet can have thousands of unknown properties, and the page's diagnostics area is a list a
// person reads, not a log. `cssDiagnosticsTruncated` says when the cap was hit, so "16 problems" is
// never mistaken for "all of them".
[[nodiscard]] std::string
canvas_report_json(const SceneFrameResult& result, std::uint32_t width, std::uint32_t height,
                   bool has_device, std::uint32_t frames,
                   const std::vector<PreviewCssDiagnostic>& css_diagnostics);

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

// What one simulated fold-posture change did (issue #408), as one JSON object on one line:
//
//   {"foldVersion":1,"changed":true,"error":""}
//
// `changed` is `PreviewSession::set_posture()`'s answer: true when a layout pass ran, false when
// the posture was already the one in force (so the page draws no frame). The layout pass completes
// inside the call, so a page that draws once on `changed` paints the new posture on its first
// frame, which is the "no frame of stale padding" rule this serializer exists to make observable.
[[nodiscard]] std::string canvas_fold_report_json(const SceneFrameResult& result, bool changed);

// What one *apply* did (issue #444: the editor's live edit), as one JSON object on one line:
//
//   {"applyVersion":1,"changed":true,"reused":false,"carried":1,"droppedComponent":0,
//    "droppedCell":0,"droppedType":0,"added":0,"frames":3,"error":""}
//
// `changed` is `CanvasSceneSession::needs_frame()` after the apply, and it is the page's whole
// decision: true means draw exactly one frame, false means the picture on the canvas is already
// the one this source describes -- which is how an auto-apply of unchanged text costs no frame.
//
// `frames` is `CanvasSceneSession::frames_painted()`, the session's own lifetime count. It is in this
// report because the live edit's contract is a *frame count* ("one frame per burst", "zero frames when
// static") and a count the page reads is evidence, where a count the engine asserts in a unit test is
// only a promise. The page surfaces it in the Canvas pane's counters, so the browser probe can check
// the rule against the same number the engine used.
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
// and the canvas pane is where a developer watches it. Before #444 the canvas had no way to show its
// own state at all -- the Scene pane's input script was the only place a cell was ever printed -- so
// a developer changing a string could not tell a carried count from a reset one. `value` is a JSON
// scalar or string; a list cell is reported as its JSON array, which is the same shape
// `snapshot_state()` uses.
[[nodiscard]] std::string canvas_apply_report_json(bool changed, bool reused,
                                                   const StateCarryReport& carry,
                                                   const ui::StateSnapshot& state,
                                                   std::uint32_t frames, std::string_view error);

} // namespace tessera::compiler::codegen::cli
