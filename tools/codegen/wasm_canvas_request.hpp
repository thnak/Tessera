// SPDX-License-Identifier: MIT

#pragma once

#include "scene_canvas_session.hpp"
#include "tessera/hal/fold_posture.hpp"

#include <string>
#include <string_view>

// The canvas module's ABI arguments, parsed in one place (issue #444).
//
// **Why this is not in `wasm_canvas_entry.cpp`.** The entry has four exports that take the same two
// things -- a fold posture string and a `(source, file, component, width, height, dpi)` tuple --
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

// Everything `CanvasSceneSession::start()` takes, from the ABI's raw arguments, or the reason it
// could not be built.
//
// `width`/`height` are the **logical** layout viewport and `dpi` only scales the backing store; the
// callers keep that distinction because `tessera_canvas_frame` derives its render extent from the
// same pair while the session exports take the logical rectangle directly.
//
// `stylesheet` is the author's CSS (issue #390), the page's second textarea, and it is part of the
// tuple for the same reason the source is: a session that started without it would lay the source
// out against the built-in sheet. Empty means "no author CSS" and keeps the built-in fallback,
// which is what a cleared textarea and a pre-#390 caller both mean.
[[nodiscard]] std::string canvas_session_source(std::string_view source, std::string_view file,
                                                std::string_view component,
                                                std::string_view posture_text,
                                                std::string_view stylesheet, float width,
                                                float height, float dpi, CanvasSessionSource& out);

} // namespace tessera::compiler::codegen::cli
