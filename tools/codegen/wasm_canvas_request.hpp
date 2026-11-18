// SPDX-License-Identifier: MIT

#pragma once

#include "scene_canvas_session.hpp"
#include "tessera/hal/fold_posture.hpp"

#include <string>
#include <string_view>

// The canvas module's ABI arguments, parsed in one place (issue #444).
//
// **Why this is not in `wasm_canvas_entry.cpp`.** The entry has exports that take the same two
// things -- a fold posture string and a `(component, width, height, dpi)` tuple --
// and each used to carry its own copy of the validation. When the live edit added a fifth caller
// (`tessera_canvas_apply`) that duplication pushed the entry past the 500-line hard limit, and the
// copy that matters is the one that could drift: a posture parsed as `book` in one export and
// silently as `flat` in another is a preview of the wrong device that looks plausible.
//
// This translation unit is host-buildable (it includes no Emscripten and no WebGPU header), so the
// parsing rules are also the part of the entry a normal Windows/Linux lane can type-check, the same
// split `canvas_report.cpp` makes for the serializers.
//
// The error strings are the module's own vocabulary, and they are deliberately the same text in
// every export: the page matches on the `TESSERA_STUDIO_FOLD_POSTURE` prefix to tell "the control
// sent something this build does not know" from "the source did not evaluate".
namespace tessera::compiler::codegen::cli {

// Parses one fold posture name into `out`, or returns the reason it could not.
//
// An empty string is `None`, not an error: the page sends "" when the current device has no fold,
// which is the ordinary case. An unknown name is an error rather than a fallback to `None`, because
// a control that appears to work while previewing an unfolded screen is the failure the posture
// slice exists to prevent.
[[nodiscard]] std::string posture_from(std::string_view text, hal::FoldPosture& out);

// Everything `CanvasSceneSession::start()` takes besides the project, from the ABI's raw arguments,
// or the reason it could not be built.
//
// `width`/`height` are the **logical** layout viewport and `dpi` only scales the backing store. The
// project itself (its sources, its stylesheets and its entry) is staged separately
// (`wasm_project_stage.hpp`) and handed to `start()` beside this.
[[nodiscard]] std::string canvas_session_source(std::string_view component,
                                                std::string_view posture_text, float width,
                                                float height, float dpi, CanvasSessionSource& out);

// One `tessera_canvas_set_viewport` request (issue #436): the logical rectangle and the DPR, or the
// reason it was refused. A non-positive extent is an error rather than a fallback, because laying a
// tree out at a degenerate rectangle and then reporting success would show the next frame at a size
// nobody asked for.
struct CanvasViewportRequest {
    float width = 0.0f;
    float height = 0.0f;
    float dpi = 1.0f;
    std::string error; // empty when the request is usable
};

[[nodiscard]] CanvasViewportRequest canvas_viewport_request(int width, int height, float dpi);

} // namespace tessera::compiler::codegen::cli
