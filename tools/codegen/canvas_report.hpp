// SPDX-License-Identifier: MIT

#pragma once

#include "scene_canvas_session.hpp"
#include "scene_frame.hpp"
#include "scene_stylesheet.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
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
[[nodiscard]] std::string canvas_report_json(const SceneFrameResult& result, std::uint32_t width,
                                             std::uint32_t height, bool has_device);

// The same report with the author stylesheet's parse problems appended (issue #390), as
//
//   "cssDiagnostics":[{"code":"TESSERA_CSS_UNKNOWN_PROPERTY","severity":"warning",
//                      "message":"unsupported property","file":"ui/Preview.css",
//                      "line":3,"column":5,"offset":42},...]
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
                   bool has_device, const std::vector<PreviewCssDiagnostic>& css_diagnostics);

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

} // namespace tessera::compiler::codegen::cli
