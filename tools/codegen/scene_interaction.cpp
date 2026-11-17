// SPDX-License-Identifier: MIT

#include "scene_evaluate.hpp"
#include "scene_interaction.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/compiler/eval/number_text.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char character : text) {
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
            if (static_cast<unsigned char>(character) < 0x20U) {
                constexpr std::string_view kHex = "0123456789abcdef";
                const auto code = static_cast<unsigned char>(character);
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

// One scalar as `{"kind":"int","value":"1"}`. The value is text in every case so a diff never has
// to reason about JSON number formatting, and so `-0`, `NaN` and a 64-bit id all survive it.
void append_scalar(std::string& out, const ui::StateScalar& scalar) {
    if (const bool* flag = std::get_if<bool>(&scalar)) {
        out += "{\"kind\":\"bool\",\"value\":";
        append_quoted(out, *flag ? "true" : "false");
    } else if (const int* whole = std::get_if<int>(&scalar)) {
        out += "{\"kind\":\"int\",\"value\":";
        append_quoted(out, std::to_string(*whole));
    } else if (const double* number = std::get_if<double>(&scalar)) {
        out += "{\"kind\":\"float\",\"value\":";
        append_quoted(out, eval::float_text(*number));
    } else {
        out += "{\"kind\":\"string\",\"value\":";
        append_quoted(out, std::get<std::string>(scalar));
    }
    out.push_back('}');
}

} // namespace

std::string_view input_script_format() noexcept {
    return "one step per line: `click <automationId>`";
}

bool parse_input_script(std::string_view text, std::vector<PreviewInputStep>& out,
                        std::string& error) {
    out.clear();
    std::size_t line_number = 0;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('\n', begin);
        const std::string_view line = text.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        ++line_number;
        begin = end == std::string_view::npos ? text.size() + 1 : end + 1;

        // Trim: a hand-written script gets trailing whitespace and CRLF from an editor.
        std::size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string_view::npos) {
            continue;
        }
        std::size_t last = line.find_last_not_of(" \t\r");
        const std::string_view trimmed = line.substr(first, last - first + 1);
        if (trimmed.front() == '#') {
            continue;
        }
        if (out.size() >= kMaxInputSteps) {
            error = "the script has more than " + std::to_string(kMaxInputSteps) + " steps";
            return false;
        }
        constexpr std::string_view kClick = "click";
        if (trimmed.rfind(kClick, 0) != 0) {
            error = "line " + std::to_string(line_number) + ": expected `click <automationId>`";
            return false;
        }
        std::string_view rest = trimmed.substr(kClick.size());
        const std::size_t start = rest.find_first_not_of(" \t");
        if (start == std::string_view::npos) {
            error = "line " + std::to_string(line_number) + ": `click` needs an automation id";
            return false;
        }
        rest = rest.substr(start);
        std::uint64_t id = 0;
        const auto [ptr, code] = std::from_chars(rest.data(), rest.data() + rest.size(), id);
        if (code != std::errc{} || ptr != rest.data() + rest.size()) {
            error = "line " + std::to_string(line_number) +
                    ": the automation id must be a decimal number, got `" + std::string(rest) + "`";
            return false;
        }
        out.push_back(PreviewInputStep{id, "click"});
    }
    return true;
}

std::string state_cells_json(const ui::StateSnapshot& state) {
    std::string out = "[";
    for (std::size_t index = 0; index < state.size(); ++index) {
        if (index != 0) {
            out.push_back(',');
        }
        const ui::StateCell& cell = state[index];
        out += "{\"name\":";
        append_quoted(out, cell.name);
        out += ",\"list\":";
        out += cell.is_list ? "true" : "false";
        out += ",\"value\":";
        if (cell.is_list) {
            out.push_back('[');
            for (std::size_t item = 0; item < cell.items.size(); ++item) {
                if (item != 0) {
                    out.push_back(',');
                }
                append_scalar(out, cell.items[item]);
            }
            out.push_back(']');
        } else {
            append_scalar(out, cell.value);
        }
        out.push_back('}');
    }
    out.push_back(']');
    return out;
}

InputRunResult run_input_script(std::string_view text, const ScenePreviewOptions& options,
                                std::string_view component, std::string_view script) {
    InputRunResult result;
    std::vector<PreviewInputStep> steps;
    std::string error;
    if (!parse_input_script(script, steps, error)) {
        result.error = std::move(error);
        return result;
    }
    auto evaluated = evaluate_source(text, options.file, options.salt, component,
                                     options.stylesheet, options.stylesheet_file);
    if (!evaluated) {
        result.error = evaluated.error().detail;
        return result;
    }

    // The same choice the snapshot and the canvas make (issue #390): the author's sheet when there
    // is one, the built-in fallback otherwise. A scripted click must resolve style against the
    // sheet the page shows, or the state cells it reports would come from a differently styled
    // tree. The viewport is the media context too (issue #436), exactly as in `prepare_scene()`.
    const ui::style::CompiledStyleSheet sheet = compile_preview_stylesheet(
        options.stylesheet,
        preview_media_context(options.width, options.height, options.dpi_scale));
    PreviewSession::Options session_options;
    session_options.width = options.width;
    session_options.height = options.height;
    session_options.dpi_scale = options.dpi_scale;
    session_options.posture = options.posture;
    PreviewSession session(evaluated->module, evaluated->selected(), sheet, session_options);
    result.built = session.built();
    if (!result.built) {
        return result;
    }

    // The state before any step: the parity runner's `initial state`, which is what proves the two
    // sides started from the same cells as well as ending on them.
    result.states.push_back(session.state());
    for (const PreviewInputStep& step : steps) {
        result.clicks.push_back(session.click(step.automation_id));
        result.states.push_back(session.state());
    }
    return result;
}

std::string scene_input_document(std::string_view text, const ScenePreviewOptions& options,
                                 std::string_view component, std::string_view script) {
    std::vector<PreviewInputStep> steps;
    std::string error;
    if (!parse_input_script(script, steps, error)) {
        // A malformed script is the caller's bug, and the document says so in the same shape: the
        // page shows `error` and the parity gate fails on the missing steps rather than passing on
        // two empty documents.
        std::string out = "{\"inputVersion\":1,\"error\":";
        append_quoted(out, error);
        out += ",\"steps\":[]}\n";
        return out;
    }
    const InputRunResult run = run_input_script(text, options, component, script);
    std::string out = "{\"inputVersion\":1";
    if (!run.built) {
        out += ",\"error\":";
        append_quoted(out, run.error.empty() ? "the source produced no scene" : run.error);
        out += ",\"steps\":[]}\n";
        return out;
    }
    out += ",\"initial\":";
    out += state_cells_json(run.states.front());
    out += ",\"steps\":[";
    for (std::size_t index = 0; index < run.clicks.size(); ++index) {
        if (index != 0) {
            out.push_back(',');
        }
        const PreviewClickResult& click = run.clicks[index];
        out += "{\"step\":";
        out += std::to_string(index);
        out += ",\"automationId\":";
        append_quoted(out, std::to_string(steps[index].automation_id));
        out += ",\"found\":";
        out += click.found ? "true" : "false";
        out += ",\"dispatched\":";
        out += click.dispatched ? "true" : "false";
        out += ",\"ran\":[";
        for (std::size_t i = 0; i < click.ran.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            append_quoted(out, click.ran[i]);
        }
        out += "],\"unrun\":[";
        for (std::size_t i = 0; i < click.unrun.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            append_quoted(out, click.unrun[i]);
        }
        out += "],\"state\":";
        out += state_cells_json(run.states[index + 1]);
        out.push_back('}');
    }
    out += "]}\n";
    return out;
}

} // namespace tessera::compiler::codegen::cli
