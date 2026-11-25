// SPDX-License-Identifier: MIT

// The author's stylesheet for the in-memory preview (issue #390). The header states the rule; this
// unit is the one place it is implemented.
//
// **Why the choice is here and not in `prepare_scene`.** Four consumers resolve a preview's style:
// `prepare_scene` (the P2 snapshot and the native frame), `CanvasSceneSession` (the browser's
// persistent session, which keeps a sheet between pointer events), `run_input_script` (the input
// gate) and `diagnose_source` (the diagnostics document). A second copy of "which sheet, and what
// are its diagnostics" in any of them would let the snapshot and the canvas paint the same source
// with different sheets, which is exactly the drift the one-pipeline rule exists to prevent.

#include "diagnostic_record.hpp"
#include "scene_evaluate.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/ui/style/media_query.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"
#include "tessera/ui/style/tokenizer.hpp"
#include "tessera/ui/style/ua_stylesheet.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::cli {
namespace {

namespace style = tessera::ui::style;

// The one suffix that makes a stylesheet a CSS module (issue #463). The codegen CLI tests the same
// string (`kModuleSuffix` in `regenerate_stylesheets.cpp`), so the preview and a native build scope
// the same set of files.
constexpr std::string_view kModuleSuffix = ".module.css";

void append(std::vector<PreviewCssDiagnostic>& out, std::string_view file, std::string_view text,
            std::string_view code, std::string message, std::size_t offset, bool warning) {
    const DiagnosticSpan span = css_span(file, text, offset);
    PreviewCssDiagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.message = std::move(message);
    diagnostic.file = std::string(file);
    diagnostic.offset = span.offset;
    diagnostic.line = span.line;
    diagnostic.column = span.column;
    diagnostic.warning = warning;
    out.push_back(std::move(diagnostic));
}

// True when `text` holds no rule and no declaration -- whitespace, or comments only. A comment-only
// sheet is "no author CSS", not "an empty author sheet": the page's textarea ships a one-line
// sample, and a visitor who clears it writes nothing, and neither may replace the built-in fallback
// with a sheet that paints nothing.
bool has_no_css(std::string_view text) {
    style::Tokenizer tokenizer(text);
    while (true) {
        const style::Token token = tokenizer.next();
        if (token.type == style::TokenType::Eof) {
            return true;
        }
        if (token.type == style::TokenType::Comment || token.type == style::TokenType::Whitespace) {
            continue;
        }
        // Any real token means the author wrote something, even if it does not parse: a sheet of
        // one stray `}` is still the author's sheet, and silently swapping in the built-in one
        // would make the preview disagree with native for a file the author can see.
        return false;
    }
}

} // namespace

// The spec's code for a diagnostic kind (docs/specs/stylesheet-v1.md section 15). `StyleDiagnostic`
// carries the kind, never the spelling, so this is the one conversion -- the same shape
// `blob_error_name()` takes in src/app/host.cpp.
const char* css_diagnostic_code(style::StyleDiagnosticKind kind) noexcept {
    switch (kind) {
    case style::StyleDiagnosticKind::UnsupportedProperty:
        // "parsed, no semantic effect": the declaration is kept but nothing consumes it. The issue
        // requires exactly this shape -- reported, not fatal -- so an author who types `flx-grow`
        // sees the typo and still sees their frame.
        return "TESSERA_CSS_UNKNOWN_PROPERTY";
    case style::StyleDiagnosticKind::UnsupportedValue:
    case style::StyleDiagnosticKind::InvalidShorthand:
        return "TESSERA_CSS_INVALID_VALUE";
    case style::StyleDiagnosticKind::InvalidAtRule:
        return "TESSERA_CSS_UNSUPPORTED_AT_RULE";
    case style::StyleDiagnosticKind::VarCycle:
        return "TESSERA_CSS_VAR_CYCLE";
    case style::StyleDiagnosticKind::UnresolvedVar:
        return "TESSERA_CSS_UNRESOLVED_VAR";
    case style::StyleDiagnosticKind::UndeclaredPart:
        return "TESSERA_CSS_UNDECLARED_PART";
    }
    return "TESSERA_CSS_PARSE_ERROR";
}

bool has_author_stylesheet(std::string_view stylesheet) {
    return !stylesheet.empty() && !has_no_css(stylesheet);
}

style::MediaContext preview_media_context(float width, float height, float dpi_scale,
                                          hal::FoldPosture posture) noexcept {
    style::MediaContext media;
    // The logical extent is the layout viewport, so the media query and the boxes it sizes are the
    // same rectangle. `dpi_scale` is a multiple of the CSS reference density of 96 dpi
    // (`hal::WindowMetrics::dpi_scale`'s meaning), which is what `resolution` compares against.
    media.viewport_width = static_cast<double>(width);
    media.viewport_height = static_cast<double>(height);
    media.resolution_dpi = 96.0 * static_cast<double>(dpi_scale > 0.0f ? dpi_scale : 1.0f);
    // The same whole-pixel extent `simulated_metrics` hands `fold_info_for`, so the segment answers
    // are derived from the fold the layout is actually given.
    const auto whole = [](float value) {
        return static_cast<std::uint32_t>(std::lround(std::max(value, 0.0f)));
    };
    return style::with_fold(media, hal::fold_info_for(posture, whole(width), whole(height)));
}

style::PreparedStyleSheet prepare_preview_stylesheet(std::string_view stylesheet) {
    // Decision 2 (issue #390) is made here and nowhere else: the author's sheet REPLACES the
    // built-in one once it holds any CSS at all.
    return style::prepare_stylesheet(has_author_stylesheet(stylesheet) ? stylesheet
                                                                       : preview_stylesheet());
}

style::CompiledStyleSheet resolve_preview_stylesheet(const style::PreparedStyleSheet& prepared,
                                                     const style::MediaContext& media) {
    return style::CompiledStyleSheet(prepared, style::Origin::Author, {}, media);
}

style::CompiledStyleSheet compile_preview_stylesheet(std::string_view stylesheet,
                                                     const style::MediaContext& media) {
    return resolve_preview_stylesheet(prepare_preview_stylesheet(stylesheet), media);
}

std::string preview_stylesheet_text(std::string_view stylesheet, std::string_view stylesheet_file,
                                    std::optional<css_module::ModuleScanError>& error) {
    error.reset();
    // The path is the project-relative one the virtual sheet is filed under, normalized exactly as
    // `evaluate_source()` files it: the scoped name is a hash of that string, so a backslash left
    // in place would scope to a name the tree never carries.
    const std::string file =
        normalize_path(stylesheet_file.empty() ? kPreviewStylesheetFile : stylesheet_file);
    if (!file.ends_with(kModuleSuffix)) {
        return std::string(stylesheet);
    }
    // The scan is the compiler's own, not a preview copy: a class the preview scopes differently
    // from what `tessera_codegen` embeds would be the same drift #497 removed from the CLI.
    const auto scanned = css_module::scan_module_css(file, stylesheet);
    if (!scanned) {
        error = scanned.error();
        return {};
    }
    return scanned->rewritten;
}

std::vector<PreviewCssDiagnostic> author_stylesheet_diagnostics(std::string_view stylesheet,
                                                                std::string_view stylesheet_file,
                                                                std::size_t limit) {
    std::vector<PreviewCssDiagnostic> out;
    if (!has_author_stylesheet(stylesheet) || limit == 0) {
        return out;
    }
    const std::string_view file =
        stylesheet_file.empty() ? kPreviewStylesheetFile : stylesheet_file;
    // The lexical error first: it is the one failure that makes the rest of the sheet unreadable,
    // and the at-rule parsers would otherwise report their own confusion further down.
    {
        style::Tokenizer tokenizer(stylesheet);
        while (out.size() < limit) {
            const style::Token token = tokenizer.next();
            if (token.type == style::TokenType::Eof) {
                break;
            }
            if (token.type == style::TokenType::BadString) {
                append(out, file, stylesheet, "TESSERA_CSS_PARSE_ERROR",
                       "unterminated string in the stylesheet", token.offset, false);
                break;
            }
        }
    }
    // The prepared sheet's own diagnostics: unknown properties, rejected values, invalid at-rules,
    // too-deep at-rule nesting, var() cycles. Preparing is what the compile below does anyway, so
    // this is a second walk of a sheet the preview was going to build either way, not a second
    // parse.
    const style::PreparedStyleSheet prepared = style::prepare_stylesheet(stylesheet);
    for (const style::StyleDiagnostic& diagnostic : prepared.diagnostics) {
        if (out.size() >= limit) {
            break;
        }
        append(out, file, stylesheet, css_diagnostic_code(diagnostic.kind), diagnostic.message,
               diagnostic.offset, true);
    }
    return out;
}

tessera::compiler::lowering::StylesheetFile
preview_stylesheet_file(std::string_view stylesheet, std::string_view stylesheet_file) {
    // Exactly one virtual file, whether or not the author wrote anything: the page owns that path,
    // so `import "./Preview.css";` is never "missing or unreadable". An empty text is what a page
    // nobody has touched yet has, and lowering accepts it (the import is recorded; the sheet has no
    // rules).
    return tessera::compiler::lowering::StylesheetFile{
        stylesheet_file.empty() ? kPreviewStylesheetFile : stylesheet_file, stylesheet};
}

style::CompiledStyleSheet PreparedPreviewSheets::resolve(const style::MediaContext& media) const {
    std::vector<style::StylesheetSource> sources;
    sources.reserve(sheets_.size() + 1);
    // The user-agent defaults first, the same sheet the app host appends: the preview and a native
    // build must agree about what an unstyled control looks like.
    sources.push_back(style::StylesheetSource{"user-agent", &style::ua_prepared_stylesheet(),
                                              style::Origin::UserAgent, false});
    for (std::size_t index = 0; index < sheets_.size(); ++index) {
        sources.push_back(
            style::StylesheetSource{paths_[index], &sheets_[index], style::Origin::Author, false});
    }
    return style::CompiledStyleSheet(std::span<const style::StylesheetSource>(sources), {}, media);
}

bool PreparedPreviewSheets::media_answer_can_change(const style::MediaContext& from,
                                                    const style::MediaContext& to) const {
    for (const style::PreparedStyleSheet& sheet : sheets_) {
        if (sheet.media_answer_can_change(from, to)) {
            return true;
        }
    }
    return false;
}

PreviewSheets prepare_preview_sheets(std::span<const SheetUse> uses, std::size_t limit) {
    PreviewSheets out;
    bool any_author = false;
    for (const SheetUse& use : uses) {
        any_author = any_author || has_author_stylesheet(use.text);
    }
    if (!any_author) {
        // No imported sheet holds CSS: the built-in sheet, and no diagnostics (a sheet the author
        // never wrote has no problems).
        out.prepared.sheets_.push_back(style::prepare_stylesheet(preview_stylesheet()));
        out.prepared.paths_.emplace_back("builtin");
        return out;
    }
    out.prepared.author_ = true;
    for (const SheetUse& use : uses) {
        if (!has_author_stylesheet(use.text)) {
            continue;
        }
        std::optional<css_module::ModuleScanError> scan_error;
        const std::string text = preview_stylesheet_text(use.text, use.path, scan_error);
        if (scan_error) {
            // Nothing from a sheet that cannot be scoped is compiled, and it is NOT replaced by the
            // built-in sheet: the author wrote CSS, and an empty sheet in its place is what keeps
            // the tree's scoped class names from meeting raw `.card` rules.
            out.prepared.sheets_.push_back(style::prepare_stylesheet(std::string_view{}));
            out.prepared.paths_.push_back(use.path);
            if (out.diagnostics.size() < limit) {
                const DiagnosticSpan span = css_span(use.path, use.text, scan_error->offset);
                PreviewCssDiagnostic diagnostic;
                diagnostic.code = scan_error->code;
                diagnostic.message = scan_error->message;
                diagnostic.file = use.path;
                diagnostic.offset = span.offset;
                diagnostic.line = span.line;
                diagnostic.column = span.column;
                diagnostic.warning = false;
                out.diagnostics.push_back(std::move(diagnostic));
            }
            continue;
        }
        out.prepared.sheets_.push_back(style::prepare_stylesheet(text));
        out.prepared.paths_.push_back(use.path);
        if (out.diagnostics.size() < limit) {
            std::vector<PreviewCssDiagnostic> found =
                author_stylesheet_diagnostics(text, use.path, limit - out.diagnostics.size());
            for (PreviewCssDiagnostic& diagnostic : found) {
                out.diagnostics.push_back(std::move(diagnostic));
            }
        }
    }
    return out;
}

} // namespace tessera::compiler::codegen::cli
