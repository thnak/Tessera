// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/fold_posture.hpp"

#include <optional>
#include <string>
#include <string_view>

// In-memory scene preview (issue #166, P2 of docs/design/reference-equantic-ui.md section 7.4): the
// browser's half of the scene snapshot. It is the sibling of `diagnose_source()`
// (tools/codegen/diagnose.cpp) and obeys the same rule: the stages run in the CLI's order over one
// in-memory source, and every value that reaches the document comes from the implementation the
// native harness already uses, never from a second copy.
//
// Stage order (mirrors regenerate.cpp's): parse -> analyse -> lower -> assign identity -> evaluate
// the component's initialState -> build the scene tree -> resolve computed style -> run Yoga layout
// -> apply bounds -> serialize the UiSnapshot. Nothing here reads the filesystem, allocates a GPU
// object, touches a clock, or depends on the locale, so it runs natively and in Wasm unchanged.
namespace tessera::compiler::codegen::cli {

// What the preview does with the source it is given. `salt` is the project salt the ids are hashed
// with (the `app.namespace` of tessera.json): the page passes the one it shows, so its `uiId`s are
// the ones the native build would use. `width`/`height` are the layout viewport in logical pixels.
//
// `posture` is the simulated foldable posture (issue #408, docs/specs/designer-studio-v1.md section
// 6). It is data like the size, never a sensor reading: `hal::fold_info_for()` turns it into the
// `FoldInfo` that goes into the `WindowMetrics` the layout and the `SafeArea` arithmetic read, so a
// previewed screen responds to a fold exactly as a device would. `FoldPosture::None` is the
// pre-#408 behaviour and produces a snapshot that contributes no insets at all.
struct ScenePreviewOptions {
    std::string_view file = "ui/Preview.tsx";
    std::string_view salt = "preview";
    float width = 320.0f;
    float height = 240.0f;
    float dpi_scale = 1.0f;
    tessera::hal::FoldPosture posture = tessera::hal::FoldPosture::None;
};

// Evaluates `text` as the single file `options.file` and returns the canonical UiSnapshot document
// of docs/specs/automation-ids-v1.md section 5.2 — exactly the bytes `snapshot_to_json()` writes,
// so it is the same artifact `tessera headless` reports, not a preview-only format. An empty
// `component` takes the first component the IR declares.
//
// Deterministic: the document is a pure function of `text`, `options` and `component`.
//
// Failure is reported in the document, not beside it: a parse error, a lowering rejection, an
// unsupported construct or an evaluation error yields a valid document of this version with an
// empty `nodes` array. A caller that needs to know why calls `diagnose_source()` on the same input,
// which is the one diagnostics serializer (docs/specs/compiler-diagnostics-v1.md). A parity gate
// that compares two empty documents must therefore declare a minimum node count, as
// scripts/ci/scene_snapshot_parity.py does.
[[nodiscard]] std::string scene_preview_document(std::string_view text,
                                                 const ScenePreviewOptions& options,
                                                 std::string_view component = {});

} // namespace tessera::compiler::codegen::cli
