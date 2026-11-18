// SPDX-License-Identifier: MIT

#include "canvas_report.hpp"
#include "css_diagnostics_report.hpp"

#include <string>
#include <string_view>
#include <variant>

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
// report (`css_diagnostics_report.hpp`): both write the project's own ASCII messages, so both need
// exactly the quote/backslash/control-character rule and nothing more. #444's state cells use the
// same encoder, so a quote in a cell value cannot break the document the page parses -- one rule,
// one function, in the file that was already the owner of it.
void append_text(std::string& out, std::string_view name, std::string_view text) {
    out += ",";
    out.push_back('"');
    out += name;
    out += "\":";
    out += quoted(text);
}

// One escaped JSON string, with no field name. The state cells and the array elements need this and
// `append_text` is the same encoder plus `"name":`, so they share `quoted()` rather than a second
// copy of the escape table.
void append_escaped(std::string& out, std::string_view text) {
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

// One state scalar as JSON. `u8string` is the JSON spelling this serializer already uses for string
// fields (a quote or a backslash in a cell value must not break the document the page parses).
void append_scalar(std::string& out, const ui::StateScalar& value) {
    if (const auto* flag = std::get_if<bool>(&value)) {
        out += *flag ? "true" : "false";
        return;
    }
    if (const auto* number = std::get_if<int>(&value)) {
        out += std::to_string(*number);
        return;
    }
    if (const auto* decimal = std::get_if<double>(&value)) {
        // A whole-valued double prints without a decimal point, so `2` reads as `2` and not
        // `2.000000`
        // -- the same choice `snapshot_state()` makes when it prints a cell.
        const bool whole = *decimal == static_cast<double>(static_cast<long long>(*decimal));
        if (whole) {
            out += std::to_string(static_cast<long long>(*decimal));
        } else {
            out += std::to_string(*decimal);
        }
        return;
    }
    append_escaped(out, std::get<std::string>(value));
}

// The state cells as one JSON array: `[{"name":"state::count","value":2}]`. A list cell is reported
// with its items and no `value`, which is the shape `ui::StateCell` itself uses (a list leaves
// `value` at its default).
void append_state(std::string& out, const ui::StateSnapshot& state) {
    out += ",\"state\":[";
    for (std::size_t index = 0; index < state.size(); ++index) {
        if (index != 0) {
            out.push_back(',');
        }
        const ui::StateCell& cell = state[index];
        out += "{\"name\":";
        append_escaped(out, cell.name);
        if (cell.is_list) {
            out += ",\"items\":[";
            for (std::size_t item = 0; item < cell.items.size(); ++item) {
                if (item != 0) {
                    out.push_back(',');
                }
                append_scalar(out, cell.items[item]);
            }
            out += "]";
        } else {
            out += ",\"value\":";
            append_scalar(out, cell.value);
        }
        out += "}";
    }
    out += "]";
}

} // namespace

std::string
canvas_report_json(const SceneFrameResult& result, std::uint32_t width, std::uint32_t height,
                   bool has_device, std::uint32_t frames,
                   const std::vector<PreviewCssDiagnostic>&
                       css_diagnostics, // The field names mirror the native CLI's printed
                                        // counters, so a parity script can compare a
                   std::uint64_t cascade_generation) { // browser report with a native dump
                                                       // without a second vocabulary.
    std::string out = "{\"canvasVersion\":1";
    append_bool(out, "hasDevice", has_device);
    append_number(out, "width", width);
    append_number(out, "height", height);
    // The session's lifetime frame count, which the live edit's contract is written in (#444): the
    // page surfaces it so a browser probe can check "one frame per burst, zero when static" against
    // the engine's own number rather than inferring it from a repaint. `cascade` is the same idea
    // for the viewport rule (#436): a resize re-resolves the cascade only when a breakpoint is
    // crossed.
    append_number(out, "frames", frames);
    append_number(out, "cascade", cascade_generation);
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
    // Keyboard focus (issue #388): who holds it, whether the ring shows, and what the page does
    // with the key it just forwarded.
    append_text(out, "focusedId", std::to_string(result.focused_id));
    append_bool(out, "focusVisible", result.focus_visible);
    append_bool(out, "focusMoved", result.focus_moved);
    append_bool(out, "handled", result.handled);
    append_bool(out, "left", result.left);
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

std::string canvas_viewport_report_json(bool changed, float width, float height, float dpi,
                                        std::uint64_t cascade_generation, std::string_view error) {
    std::string out = "{\"viewportVersion\":1";
    append_bool(out, "changed", changed);
    append_number(out, "width", static_cast<std::uint64_t>(width > 0.0f ? width : 0.0f));
    append_number(out, "height", static_cast<std::uint64_t>(height > 0.0f ? height : 0.0f));
    // The DPR is a scale, not a count, so it keeps its fraction (`1.5` is a real device).
    out += ",\"dpr\":";
    out += std::to_string(dpi > 0.0f ? dpi : 1.0f);
    append_number(out, "cascadeGeneration", cascade_generation);
    append_text(out, "error", error);
    out += "}\n";
    return out;
}

std::string canvas_apply_report_json(bool changed, bool reused, const StateCarryReport& carry,
                                     const ui::StateSnapshot& state, std::uint32_t frames,
                                     std::string_view error,
                                     const std::vector<std::string>& components,
                                     std::string_view component) {
    std::string out = "{\"applyVersion\":1";
    append_bool(out, "changed", changed);
    append_bool(out, "reused", reused);
    append_number(out, "carried", carry.carried);
    append_number(out, "droppedComponent", carry.dropped_component);
    append_number(out, "droppedCell", carry.dropped_cell);
    append_number(out, "droppedType", carry.dropped_type);
    append_number(out, "added", carry.added);
    append_number(out, "frames", frames);
    append_state(out, state);
    // The entry's declared components and the one previewed (#391): the page's dropdown.
    out += ",\"components\":[";
    bool first = true;
    for (const std::string& name : components) {
        append_string_element(out, first, name);
        first = false;
    }
    out += "]";
    append_text(out, "component", component);
    append_text(out, "error", error);
    out += "}\n";
    return out;
}

} // namespace tessera::compiler::codegen::cli
