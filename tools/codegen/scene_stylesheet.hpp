// SPDX-License-Identifier: MIT

#pragma once

#include "scene_project.hpp"
#include "tessera/compiler/css_module/module_scan.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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

// The spec's `TESSERA_CSS_*` spelling of a diagnostic kind (stylesheet-v1 section 15). The one
// conversion: the sheet's own diagnostics and an override's refusal (`scene_override.hpp`) both
// speak it, so a page never meets two spellings of one problem.
[[nodiscard]] const char*
css_diagnostic_code(tessera::ui::style::StyleDiagnosticKind kind) noexcept;

// True when `stylesheet` holds CSS the author wrote. Blank text, whitespace and comments only are
// all "no author CSS", so every caller written before issue #390 keeps the built-in fallback and
// the same bytes.
[[nodiscard]] bool has_author_stylesheet(std::string_view stylesheet);

// The media context a preview resolves `@media` against (issue #436): the **logical** layout
// viewport and the DPI scale as CSS dpi (96 at scale 1, so a 2x device answers
// `(min-resolution: 2dppx)`).
//
// The preview has no OS window, so this is the only place its device preset becomes a media query's
// answer. It is derived from the same two numbers the layout engine is given, so a
// `@media (max-width: 600px)` rule and the box it styles cannot disagree. Before this existed the
// previews resolved against `MediaContext`'s default 1280x720 rectangle, which is why a device
// preset could not trigger a breakpoint.
//
// `posture` is the simulated fold (ADR-074): it becomes `device-posture` and the viewport-segment
// features through the same `FoldInfo` the layout uses (`hal::fold_info_for`) and the one
// `with_fold` mapping the app host calls, so a previewed foldable and a device cannot answer a
// fold query differently. The default is no fold.
[[nodiscard]] tessera::ui::style::MediaContext
preview_media_context(float width, float height, float dpi_scale,
                      tessera::hal::FoldPosture posture = tessera::hal::FoldPosture::None) noexcept;

// The media-independent form of the preview's sheet (ADR-048 decision 2): prepared once, resolved
// against each viewport. Kept separate from the compiled form so a viewport change re-resolves the
// same data instead of parsing CSS again - the resolve step `AppHost` uses, in the preview.
[[nodiscard]] tessera::ui::style::PreparedStyleSheet
prepare_preview_stylesheet(std::string_view stylesheet);

// The program's stylesheets, prepared once and resolved per viewport: the preview's sheet when the
// program imports several (a project), and the built-in sheet when it imports none.
//
// **One rule, stated once** (issue #390 decision 2, extended to a project): the author's sheets
// REPLACE the built-in one as soon as any imported sheet holds CSS, and the built-in sheet is used
// only when none does. The sheets are combined by `CompiledStyleSheet`'s ordered cascade, so they
// keep one global source order and a stray `}` or an unterminated string in one file cannot swallow
// the next -- which concatenating the texts would have allowed.
struct PreviewSheets;

class PreparedPreviewSheets {
public:
    PreparedPreviewSheets() = default;

    // Resolves `@media` against `media`; nothing is tokenized or parsed again.
    [[nodiscard]] tessera::ui::style::CompiledStyleSheet
    resolve(const tessera::ui::style::MediaContext& media) const;
    // True when moving between the two contexts can change what any sheet's `@media` answers.
    [[nodiscard]] bool media_answer_can_change(const tessera::ui::style::MediaContext& from,
                                               const tessera::ui::style::MediaContext& to) const;
    // True when the author's sheets are in force rather than the built-in one.
    [[nodiscard]] bool author() const noexcept {
        return author_;
    }

private:
    friend PreviewSheets prepare_preview_sheets(std::span<const SheetUse> uses, std::size_t limit);

    std::vector<tessera::ui::style::PreparedStyleSheet> sheets_;
    std::vector<std::string> paths_;
    bool author_ = false;
};

// The prepared sheets and what is wrong with them, from one pass: a diagnostic describes a rule the
// sheet resolved from this same preparation, so the pane cannot report one sheet's problems beside
// another sheet's pixels. `diagnostics` is bounded by `limit` across all sheets, in sheet order.
struct PreviewSheets {
    PreparedPreviewSheets prepared;
    std::vector<PreviewCssDiagnostic> diagnostics;
};

[[nodiscard]] PreviewSheets prepare_preview_sheets(std::span<const SheetUse> uses,
                                                   std::size_t limit = kMaxReportedCssDiagnostics);

// The compiled form of an already prepared sheet for one media context: `@media` is evaluated
// against `media`, and nothing is tokenized or parsed again.
[[nodiscard]] tessera::ui::style::CompiledStyleSheet
resolve_preview_stylesheet(const tessera::ui::style::PreparedStyleSheet& prepared,
                           const tessera::ui::style::MediaContext& media);

// The sheet the preview lays out and paints with: the author's when `has_author_stylesheet()` is
// true, the built-in `preview_stylesheet()` otherwise. Never a combination of the two.
[[nodiscard]] tessera::ui::style::CompiledStyleSheet
compile_preview_stylesheet(std::string_view stylesheet,
                           const tessera::ui::style::MediaContext& media);

// The text the preview compiles, and the one decision "is this a CSS module" is made in (issue
// #463). The author's bytes are returned unchanged unless `stylesheet_file` names a
// `*.module.css`; a module sheet is returned as its scanned rewrite, so the class names the lowered
// tree carries on `className` are the selectors the compiled sheet defines.
//
// **The suffix decides, not the form of the import.** A sheet imported for effect and one imported
// for its class map are the same file, so how it was imported says nothing about whether its class
// names are scoped. The codegen CLI scopes by the same suffix
// (`regenerate_stylesheets.cpp`), which is what keeps the preview and a native build of the same
// file identical. An empty `stylesheet_file` falls back to `kPreviewStylesheetFile`, a plain name,
// so every caller written before #463 compiles exactly the bytes it did before.
//
// `error` is set -- and the result left empty -- when a module sheet cannot be scanned. The empty
// result is "this sheet compiles to no rules"; it is NOT "no author CSS", so a caller must not hand
// it to `compile_preview_stylesheet()`, whose empty-input rule would resurrect the built-in sheet
// over an author sheet that exists but is broken. Report the error as a `PreviewCssDiagnostic` and
// compile an empty sheet with `resolve_preview_stylesheet()`.
[[nodiscard]] std::string
preview_stylesheet_text(std::string_view stylesheet, std::string_view stylesheet_file,
                        std::optional<css_module::ModuleScanError>& error);

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
