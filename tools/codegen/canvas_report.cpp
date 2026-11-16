// SPDX-License-Identifier: MIT

#include "canvas_report.hpp"

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

// A string field, escaped for JSON. The only text that reaches here is this module's own ASCII
// error messages, so escaping quotes, backslashes and control characters is the whole job; pulling
// a general encoder into a module this small would be a second implementation of a solved problem.
void append_escaped(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char character : text) {
        const auto code = static_cast<unsigned char>(character);
        switch (character) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (code < 0x20U) {
                // U+0000..U+001F must not appear raw in a JSON string.
                constexpr char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[code >> 4U]);
                out.push_back(kHex[code & 0x0FU]);
            } else {
                out.push_back(character);
            }
            break;
        }
    }
    out.push_back('"');
}

void append_text(std::string& out, std::string_view name, std::string_view text) {
    out += ",";
    out.push_back('"');
    out += name;
    out += "\":";
    append_escaped(out, text);
}

// One element of a JSON string array, with the comma the caller does not have to place.
void append_string_element(std::string& out, bool first, std::string_view text) {
    if (!first) {
        out.push_back(',');
    }
    append_escaped(out, text);
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
    append_number(out, "vectorsSkipped", result.vectors_skipped);
    append_number(out, "imagesSkipped", result.images_skipped);
    append_bool(out, "pixelsAvailable", result.pixels_available);
    append_text(out, "error", result.error);
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

} // namespace tessera::compiler::codegen::cli
