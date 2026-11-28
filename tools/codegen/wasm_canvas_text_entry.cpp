// SPDX-License-Identifier: MIT

// The canvas module's keyboard and text exports (issues #388 and #601): `tessera_canvas_key` and
// `tessera_canvas_text`. Split out of `wasm_canvas_entry.cpp`, which owns the module's device and
// session and was over the source-size limit; these two take the session through
// `canvas_query_session()` and never create, replace or reset it.

#include "canvas_report.hpp"
#include "scene_canvas_session.hpp"
#include "wasm_canvas_support.hpp"

#include <emscripten/emscripten.h>
#include <string_view>

namespace tessera::compiler::codegen::cli {

// Defined in `wasm_canvas_entry.cpp`, which owns the module's state.
[[nodiscard]] CanvasSceneSession* canvas_query_session();
[[nodiscard]] char* canvas_duplicate_out(std::string_view text);

} // namespace tessera::compiler::codegen::cli

namespace {

namespace cli = tessera::compiler::codegen::cli;

const char* duplicate_out(std::string_view text) {
    return cli::canvas_duplicate_out(text);
}
} // namespace

extern "C" {

// Routes one keyboard event into the running session (issue #388, focus) and returns the same
// document `tessera_canvas_pointer` does, with the focus fields filled.
//   kind       "down" | "up"            a key event; "blur" drops focus (the element lost the
//   keyboard) key        a W3C `KeyboardEvent.key` ("Tab", "Enter", " ", "ArrowDown", "a")
//   modifiers  a `ui::Modifier` mask: Shift 1, Control 2, Alt 4, Meta 8 (the page's `shiftKey`
//   etc.) repeat     non-zero for an auto-repeat of a held key
//
// Tab and Shift+Tab move focus through the focusable controls in tree order and **release it at
// either end** (`left: true`, `handled: false`) rather than wrapping, so the page leaves the key
// alone and the browser moves on to the next element: a canvas must not be a keyboard trap. Enter
// and Space activate the focused button through the router's click synthesis. `handled` tells the
// page whether to call `preventDefault`. An unknown `kind` or `key` answers a well-formed report
// with nothing done, like the pointer export.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_key(const char* kind, const char* key,
                                                    int modifiers, int repeat) {
    cli::CanvasPointerResult result;
    cli::CanvasSceneSession* const session = cli::canvas_query_session();
    if (session == nullptr || !session->started()) {
        return duplicate_out(cli::canvas_pointer_report_json(result));
    }
    const std::string_view name = kind != nullptr ? std::string_view(kind) : std::string_view();
    if (name == "blur") {
        return duplicate_out(cli::canvas_pointer_report_json(session->blur()));
    }
    bool known = false;
    const cli::PreviewInput::Kind parsed = cli::key_kind_from(name, known);
    const std::uint32_t code =
        cli::key_code_from_name(key != nullptr ? std::string_view(key) : std::string_view());
    if (!known || code == 0) {
        return duplicate_out(cli::canvas_pointer_report_json(result));
    }
    result = session->key(parsed, code, modifiers > 0 ? static_cast<std::uint32_t>(modifiers) : 0U,
                          repeat != 0);
    return duplicate_out(cli::canvas_pointer_report_json(result));
}

// Text for the focused `TextInput`/`TextArea` (issue #601, docs/design/canvas-text-input.md).
// `kind` is
// `"commit"` (typed or pasted text, an IME result), `"start"`, `"update"` or `"end"` (the IME
// preedit lifecycle), or `"copy"` / `"cut"` (`text` is ignored; the report's `text` is the
// selection). The report is the key report: `handled` says the field took it, `textFocus`, `caret`
// and `fieldBox` place the page's hidden editable element. An unknown kind answers a well-formed
// report with nothing done.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_text(const char* kind, const char* text) {
    cli::CanvasPointerResult result;
    cli::CanvasSceneSession* const session = cli::canvas_query_session();
    if (session == nullptr || !session->started()) {
        return duplicate_out(cli::canvas_pointer_report_json(result));
    }
    const std::string_view name = kind != nullptr ? std::string_view(kind) : std::string_view();
    if (name == "copy" || name == "cut") {
        return duplicate_out(
            cli::canvas_pointer_report_json(session->text_clipboard(name == "cut")));
    }
    bool known = false;
    const cli::PreviewInput::Kind parsed = cli::text_kind_from(name, known);
    if (!known) {
        return duplicate_out(cli::canvas_pointer_report_json(result));
    }
    result = session->text(parsed, text != nullptr ? std::string_view(text) : std::string_view());
    return duplicate_out(cli::canvas_pointer_report_json(result));
}

} // extern "C"
