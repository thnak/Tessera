// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The author's stylesheet, as the preview consumes it (issue #390).
//
// **One rule, stated once.** The preview compiles `preview_stylesheet()` only when the author
// supplied no CSS at all; otherwise it compiles exactly the author's text and the built-in sheet is
// never mentioned again (the owner's decision 2: the built-in sheet is REPLACED by user code, not
// layered under it). "Supplied no CSS at all" is about what the author wrote, not about what
// survived validation: a sheet with one bad rule is still the author's sheet, so an invalid
// declaration cannot resurrect the built-in blue Buttons while the page shows the error that
// explains it.
//
// This file exists rather than three copies of the choice because `prepare_scene` (the snapshot and
// the native frame), `CanvasSceneSession` (the browser's persistent session), the input path and
// the diagnostics document all need the same answer, and a preview whose snapshot and canvas ran
// different sheets would be two previews.
//
// It deliberately does NOT include `scene_preview.hpp`: the diagnostics module is built without the
// engine's HAL, and taking two string_views keeps this usable from there.
namespace tessera::compiler::codegen::cli {

// The preview's built-in stylesheet (defined in `scene_prepare.cpp`): sizes and the dark palette
// that give the intrinsic element kinds something to paint. Declared here because this file is what
// decides whether it is used at all.
[[nodiscard]] std::string_view preview_stylesheet() noexcept;

// The virtual project file an author stylesheet is filed under when a caller names none. A source's
// `import "./Preview.css";` resolves to it.
inline constexpr std::string_view kPreviewStylesheetFile = "ui/Preview.css";

// The most problems any preview report carries. A stylesheet can have thousands of unknown
// properties, and the page's diagnostics area is a list a person reads, not a log; a report that is
// bounded is what keeps it readable. The reports say when the cap was hit, so "16 problems" is
// never mistaken for "all of them".
inline constexpr std::size_t kMaxReportedCssDiagnostics = 16;

// One stylesheet diagnostic in the form the page's diagnostics area wants: the spec's stable code
// (docs/specs/stylesheet-v1.md section 15), the message, the virtual file it came from, and a
// 1-based line/column derived from the engine's byte offset.
//
// The engine reports offsets because that is what a tokenized parse has; line and column are
// derived here, once, from the same offset, so the page and the native build cannot disagree about
// where a problem is. `column` counts bytes, which equals code points for the ASCII a CSS selector
// and property name are made of (the `css_span()` rule in tools/codegen/diagnostic_record.hpp).
struct PreviewCssDiagnostic {
    std::string code;
    std::string message;
    std::string file;
    std::uint32_t offset = 0;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    // False for the diagnostics that make part of the sheet unreadable (an unterminated string),
    // true for the ones that only mean a rule was dropped. The page shows both; neither blanks the
    // frame.
    bool warning = true;
};

// True when `stylesheet` holds CSS the author wrote. Blank text, whitespace and comments only are
// all "no author CSS", so every caller written before issue #390 keeps the built-in fallback and
// the same bytes.
[[nodiscard]] bool has_author_stylesheet(std::string_view stylesheet);

// The sheet the preview lays out and paints with: the author's when `has_author_stylesheet()` is
// true, the built-in `preview_stylesheet()` otherwise. Never a combination of the two.
[[nodiscard]] tessera::ui::style::CompiledStyleSheet
compile_preview_stylesheet(std::string_view stylesheet);

// The author sheet's diagnostics, in source order, bounded by `limit`. Empty when there is no
// author CSS, because a stylesheet the author never wrote cannot have problems.
[[nodiscard]] std::vector<PreviewCssDiagnostic>
author_stylesheet_diagnostics(std::string_view stylesheet, std::string_view stylesheet_file,
                              std::size_t limit = 16);

// The `import "./Preview.css";` target: the textarea is the virtual file `stylesheet_file`, so
// lowering resolves that specifier against this text instead of reporting it as a missing file.
// There is always exactly one virtual stylesheet (possibly empty), which is why a source that
// imports it never fails on an unreadable path -- the page owns that file.
//
// The result views its two arguments, which must outlive it. One entry, so a caller passes it
// straight to `lower_project`'s stylesheet parameter.
[[nodiscard]] tessera::compiler::lowering::StylesheetFile
preview_stylesheet_file(std::string_view stylesheet, std::string_view stylesheet_file);

} // namespace tessera::compiler::codegen::cli
