// SPDX-License-Identifier: MIT

#include "canvas_report.hpp"
#include "css_diagnostics_report.hpp"

#include <string>
#include <string_view>

namespace tessera::compiler::codegen::cli {
namespace {

void append_number(std::string& out, std::string_view name, std::uint64_t value) {
    out += ",";
    out.push_back('"');
    out += name;
    out += "\":";
    out += std::to_string(value);
}

// A string field, escaped for JSON. One implementation, shared with the stylesheet diagnostics
// report
// (`css_diagnostics_report.hpp`): both write the project's own ASCII messages, so both need exactly
// the quote/backslash/control-character rule and nothing more.
void append_text(std::string& out, std::string_view name, std::string_view text) {
    out += ",";
    out.push_back('"');
    out += name;
    out += "\":";
    out += quoted(text);
}

// One element of a JSON string array, with the comma the caller does not have to place.
void append_string_element(std::string& out, bool first, std::string_view text) {
    if (!first) {
        out.push_back(',');
    }
    out += quoted(text);
}

void append_bool(std::string& out, std::string_view name, bool value) {
    out += ",";
    out.push_back('"');
    out += name;
    out += "\":";
    out += value ? "true" : "false";
}

} // namespace

std::string canvas_report_json(const SceneFrameResult& result, std::uint32_t width,
                               std::uint32_t height, bool has_device) {
    return canvas_report_json(result, width, height, has_device, {});
}

std::string canvas_report_json(const SceneFrameResult& result, std::uint32_t width,
                               std::uint32_t height, bool has_device,
                               const std::vector<PreviewCssDiagnostic>& css_diagnostics) {
    // The field names mirror the native CLI's printed counters, so a parity script can compare a
    // browser report with a native dump without a second vocabulary.
    std::string out = "{\"canvasVersion\":1";
    append_bool(out, "hasDevice", has_device);
    append_number(out, "width", width);
    append_number(out, "height", height);
    append_number(out, "nodes", result.nodes);
    append_number(out, "drawCalls", result.draw_calls);
    append_number(out, "vertices", result.vertex_count);
    append_number(out, "backgrounds", result.backgrounds);
    append_number(out, "borders", result.borders);
    append_number(out, "texts", result.texts);
    append_number(out, "textSkipped", result.text_skipped);
    // The bounded atlas's own failure counter (#387): non-zero means the frame's text was measured
    // and advanced correctly but that many glyphs' ink is absent, which is a font/atlas fact rather
    // than a stylesheet bug. Reported next to `textSkipped` so the two are never confused - one
    // says "no font, no text", the other says "text, but not all of its pixels".
    append_number(out, "glyphsDropped", result.glyphs_dropped);
    append_number(out, "vectorsSkipped", result.vectors_skipped);
    append_number(out, "imagesSkipped", result.images_skipped);
    append_bool(out, "pixelsAvailable", result.pixels_available);
    append_text(out, "error", result.error);
    // The author stylesheet's parse problems (issue #390), appended so the report keeps its version
    // (see the header). A bad rule is reported here, never as `error`: `error` means "no frame",
    // and the whole point of the tolerant parser is that a bad rule costs that rule and nothing
    // else.
    out += ",\"cssDiagnostics\":[";
    std::size_t written = 0;
    for (const PreviewCssDiagnostic& diagnostic : css_diagnostics) {
        if (written == kMaxReportedCssDiagnostics) {
            break;
        }
        if (written != 0) {
            out.push_back(',');
        }
        out += "{\"code\":";
        out += quoted(diagnostic.code);
        out += ",\"severity\":";
        out += quoted(diagnostic.warning ? "warning" : "error");
        out += ",\"message\":";
        out += quoted(diagnostic.message);
        out += ",\"file\":";
        out += quoted(diagnostic.file);
        append_number(out, "line", diagnostic.line);
        append_number(out, "column", diagnostic.column);
        append_number(out, "offset", diagnostic.offset);
        out.push_back('}');
        ++written;
    }
    out += "]";
    append_bool(out, "cssDiagnosticsTruncated", css_diagnostics.size() > written);
    out += "}\n";
    return out;
}

std::string canvas_pointer_report_json(const CanvasPointerResult& result) {
    std::string out = "{\"pointerVersion\":1";
    append_bool(out, "dispatched", result.dispatched);
    // The page's entire decision, so it is a boolean and not a dirty mask: a mask would invite the
    // page to reason about domains it does not own.
    append_bool(out, "changed", result.changed);
    append_text(out, "targetId", std::to_string(result.target_id));
    append_text(out, "hoveredId", std::to_string(result.hovered_id));
    append_text(out, "activeId", std::to_string(result.active_id));
    // Handler ids the dispatch ran and could not run, so "the button did nothing" is attributable:
    // an empty `ran` with a non-empty `unrun` is a payload-taking handler this evaluator cannot
    // execute, not a hit-testing bug.
    out += ",\"ran\":[";
    for (std::size_t index = 0; index < result.ran.size(); ++index) {
        append_string_element(out, index == 0, result.ran[index]);
    }
    out += "],\"unrun\":[";
    for (std::size_t index = 0; index < result.unrun.size(); ++index) {
        append_string_element(out, index == 0, result.unrun[index]);
    }
    out += "]}";
    out += "\n";
    return out;
}

std::string canvas_fold_report_json(const SceneFrameResult& result, bool changed) {
    // Versioned like the other two reports, so a page built against one module revision can tell an
    // older artifact apart instead of reading a missing field as "no change".
    std::string out = "{\"foldVersion\":1";
    append_bool(out, "changed", changed);
    append_text(out, "error", result.error);
    out += "}\n";
    return out;
}

} // namespace tessera::compiler::codegen::cli
