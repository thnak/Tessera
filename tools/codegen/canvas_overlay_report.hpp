// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"

#include <string>

// The preview's debug overlay as the page receives it (issue #433, `designer-studio-v1.md` section
// 6's "Safe-area and cutout overlay" row and section 6.1).
//
// **Why the engine writes this document and the page does not.** The overlay draws the hinge band
// and the safe-area insets the previewed app was actually laid out against. The page knows the
// posture it asked for, but not the `FoldInfo` that posture resolved to - and a band drawn from the
// page's own arithmetic would be a second mapping that can disagree with the layout it is supposed
// to explain. So the answer comes from `CanvasSceneSession::metrics()`, the `WindowMetrics`
// snapshot the live tree was laid out with, through the same `hal::fold_info_for()` the layout
// consumed.
//
// One JSON object on one line:
//
//   {"overlayVersion":1,"width":390,"height":844,
//    "foldState":"half-opened","foldOrientation":"vertical","foldOcclusion":"full",
//    "separating":true,"hingeAngle":-1,
//    "hinge":{"x":183,"y":0,"width":24,"height":844},
//    "insets":{"left":0,"top":0,"right":0,"bottom":0},
//    "ime":{"left":0,"top":0,"right":0,"bottom":0},
//    "gestures":{"left":0,"top":0,"right":0,"bottom":0},
//    "cutouts":[{"x":0,"y":4,"width":120,"height":32}]}
//
// `hinge` has zero area when there is no fold, which is the page's "draw no band" signal rather
// than a second boolean to keep in step with it. The state names are the wire vocabulary
// `tessera/hal/fold_posture.hpp` and `designer-protocol-v1.md` section 8 already use
// (`none`, `flat`, `half-opened`).
//
// **Nothing here is dispatched.** The document is a query over a snapshot: reading it moves no
// pseudo-class, marks no dirty domain and owes no frame, so arming the overlay cannot change the
// picture it is drawing over.
namespace tessera::compiler::codegen::cli {

// The overlay document for one `WindowMetrics`. Pure over its argument: no device, no session, no
// clock, which is what lets the host lanes test the round trip without Emscripten.
[[nodiscard]] std::string canvas_overlay_report_json(const hal::WindowMetrics& metrics);

} // namespace tessera::compiler::codegen::cli
