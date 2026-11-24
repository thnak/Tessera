// The browser entry point of the WebGPU canvas preview (issues #167, #388, #387, #408, #444).
//
// Every export is specified in `docs/design/compiler-wasm.md` section 10, which is the normative
// source for the module ABI, with the canvas session's own table in
// `docs/design/canvas-input-session.md` section 5: `tessera_webgpu_set_device`,
// `tessera_canvas_apply`, `tessera_canvas_pointer`, `tessera_canvas_pick`,
// `tessera_canvas_style`, `tessera_canvas_patch`, `tessera_canvas_set_fold`,
// `tessera_canvas_set_viewport`,
// `tessera_canvas_set_safe_area`, `tessera_canvas_draw`, `tessera_canvas_reset`,
// `tessera_canvas_set_font` and `tessera_canvas_font_status`, plus the four
// shared `tessera_project_*` exports (`wasm_project_exports.cpp`) that stage the project `apply`
// evaluates. Read that before changing a signature; it also carries the "why Emscripten only"
// rationale and the verified-status record.
//
// The one rule that lives here rather than in the doc, because it is a property of this file: the
// exports below share one anonymous-namespace device and session, so they are not splittable into
// separate translation units without exposing that state. What *is* split out is the argument
// parsing (`wasm_canvas_request.cpp`), because that is where a signature change lands and where a
// drift between two exports would show as a preview of the wrong device.
//
// Notably: coordinates arriving from the page are **logical pixels**, and the CSS-to-logical rule
// is documented on `tessera_canvas_pointer` itself -- it is not "divide by devicePixelRatio".

#include "canvas_report.hpp"
#include "preview_text.hpp"
#include "scene_canvas_session.hpp"
#include "scene_frame.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tessera/rendering/webgpu/webgpu_device.hpp"
#include "wasm_canvas_font.hpp"
#include "wasm_canvas_paint.hpp"
#include "wasm_canvas_request.hpp"
#include "wasm_canvas_safe_area.hpp"
#include "wasm_canvas_support.hpp"
#include "wasm_project_stage.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <emscripten/emscripten.h>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
namespace {

namespace cli = tessera::compiler::codegen::cli;
namespace rhi = tessera::rendering::rhi;
namespace webgpu = tessera::rendering::webgpu;

// The device the page handed over. One per module instantiation: the page requests an adapter and a
// device once and reuses them across edits, so the module keeps the wrapper for its lifetime. A
// second handover replaces it (a page that lost and re-acquired the device).
std::unique_ptr<rhi::Device> g_device;

char* duplicate_out(std::string_view text) {
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, text.data(), text.size());
    out[text.size()] = '\0';
    return out;
}

// The same report with a session's already-collected diagnostics (a Run, an Apply or a Draw does
// not re-prepare the sheet, so it reports what `start()` found), the session's own frame count, and
// its cascade generation (#436: the counter that says whether a resize re-resolved `@media`).
std::string report_with_css(const cli::SceneFrameResult& result, rhi::Extent2D extent,
                            const std::vector<cli::PreviewCssDiagnostic>& css,
                            std::uint32_t frames = 0, std::uint64_t cascade = 0) {
    return cli::canvas_report_json(result, extent.width, extent.height, true, frames, css, cascade);
}

// The persistent session (#388): one evaluation the pointer events feed and every later frame
// paints. It is created by the first `tessera_canvas_apply` and replaced by the next one, which
// carries the state cells across the edit (`scene_canvas_session.hpp`). Declared here rather than
// beside `g_device` because it must die before any device it painted through.
std::unique_ptr<cli::CanvasSceneSession> g_session;

} // namespace

namespace tessera::compiler::codegen::cli {

// The running session, for the query exports that live in `wasm_canvas_query.cpp` (issue #171's
// `source.patch` slice split them out when this entry passed the source-size limit). The queries
// read the session and answer a value; they never create, replace or reset it, so a pointer is
// enough and the entry keeps sole ownership.
CanvasSceneSession* canvas_query_session() {
    return g_session.get();
}

char* canvas_duplicate_out(std::string_view text) {
    return duplicate_out(text);
}

} // namespace tessera::compiler::codegen::cli

extern "C" {

// `device` and `queue` are Emscripten WebGPU handles (integers from `WebGPU.importJsDevice` and
// `WebGPU.importJsQueue`), not JS objects: a JS object passed here is coerced to 0 and would look
// like a null handle. Returns 1 when the device was wrapped, 0 for a null handle, -1 when wrapping
// threw, so the page can say why instead of showing a bare "no device" later.
EMSCRIPTEN_KEEPALIVE int tessera_webgpu_set_device(void* device, void* queue) {
    g_device.reset();
    if (device == nullptr || queue == nullptr) {
        return 0;
    }
    try {
        g_device = webgpu::create_device(device, queue);
    } catch (...) {
        // A module that cannot wrap the handles keeps answering "no device" rather than aborting
        // the page; the page then shows its unsupported state.
        g_device.reset();
        return -1;
    }
    return 1;
}

// Applies the staged project to the RUNNING session (issue #444: the page's live edit), without
// resetting it. The page stages the project first (`tessera_project_begin` /
// `tessera_project_add_source` / `tessera_project_add_stylesheet` / `tessera_project_set_entry`);
// this evaluates it from its entry, with the stylesheets the program imports.
//
// It differs from a plain start in two ways the live edit needs and a button does not:
//
//  1. **It reports `changed`.** An auto-apply of a project that did not change must cost no frame,
//     so this answers the session's own `needs_frame()` and the page draws only when it is true. A
//     `reused` apply (identical project) is therefore `changed:false` and zero frames.
//  2. **It reports the carry split.** `carried`/`droppedComponent`/`droppedCell`/`droppedType` come
//     from `StateCarryReport` (`scene_state.hpp`), so the page can say which cells survived the
//     edit and, when a component was removed, that the spec's reset case happened rather than a
//     silent loss.
//
// A project that does not evaluate leaves the running evaluation and its picture untouched (see
// `CanvasSceneSession::start`), and this returns `error` with `changed` as it already was: the page
// shows the diagnostic and draws nothing, which is what keeps the canvas from blanking
// mid-keystroke. A project with no entry is that same error, never a preview of some other file.
//
// `width`/`height` are the logical viewport and `dpi` the DPR. They are NOT part of the tuple that
// decides reuse (issue #436): a toolbar change re-lays the running tree out in place. A font that
// arrived after the last evaluation does re-evaluate.
//
// `tessera_canvas_draw` takes no strings at all -- it paints the tree the last apply left -- so
// the project and its stylesheets have nowhere to go but here.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_apply(const char* component,
                                                      const char* fold_posture, int width,
                                                      int height, float dpi) {
    const auto extent = rhi::Extent2D{static_cast<std::uint32_t>(width > 0 ? width : 0),
                                      static_cast<std::uint32_t>(height > 0 ? height : 0)};
    if (g_device == nullptr) {
        cli::SceneFrameResult none;
        none.error = "no WebGPU device: call tessera_webgpu_set_device first";
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, false));
    }
    if (g_session == nullptr) {
        // No running evaluation yet (the first automatic apply): this is a start, carrying nothing.
        // Treated exactly as an apply with no carry, so the page has one code path for both.
        g_session = std::make_unique<cli::CanvasSceneSession>();
    }
    const cli::ProjectStage& stage = cli::project_stage();
    if (!stage.has_entry()) {
        cli::SceneFrameResult none;
        none.error = "TESSERA_STUDIO_NO_ENTRY: the project names no entry file";
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, true));
    }
    cli::CanvasSessionSource options;
    const std::string request_error = cli::canvas_session_source(
        component != nullptr ? std::string_view(component) : std::string_view(),
        fold_posture != nullptr ? std::string_view(fold_posture) : std::string_view(),
        static_cast<float>(extent.width), static_cast<float>(extent.height), dpi, options);
    if (!request_error.empty()) {
        cli::SceneFrameResult none;
        none.error = request_error;
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, true));
    }

    // The posture and the viewport are not part of the tuple `start()` compares to decide reuse
    // (they are layout inputs, not source), so a reused apply re-lays the running tree out at the
    // new ones - in ONE pass, inside that call (issues #408 and #436). `start()` does it itself,
    // which is why there is no second call here: a posture or size change keeps hover, focus and
    // the state cells, and the answer this report needs is the session's own `needs_frame()`.
    const std::string error = g_session->start(stage.input(), options);
    return duplicate_out(cli::canvas_apply_report_json(
        g_session->needs_frame(), g_session->reused(), g_session->carry_report(),
        g_session->state(), g_session->frames_painted(), error, g_session->entry_components(),
        g_session->component_name()));
}

// Routes one pointer event into the running session and returns what it did. The page calls this
// from its pointer listeners and then draws at most one frame, and only when `changed` is true.
//
//   kind    "move" | "down" | "up" | "cancel"  (the `InputRouter`'s own gesture vocabulary)
//   x, y    the pointer position in **logical** (layout viewport) pixels
//   button  the pointer event's `button`; 0 is primary. Only the primary button acts in this slice.
//
// **The coordinate rule, which is the part that goes wrong.** `x`/`y` are logical pixels, not CSS
// pixels and not device pixels, and the conversion belongs to the page because only it can read the
// CSS box:
//
//     const rect = canvas.getBoundingClientRect();       // CSS box, what the user sees
//     const logicalWidth = widthArgumentOfLastRun;       // the viewport the layout used
//     const x = (event.clientX - rect.left) * (logicalWidth / rect.width);
//
// Divide by the canvas' **CSS-to-logical ratio**, never by `devicePixelRatio`. The backing store is
// `logical * dpr` and the CSS box is the logical size, so `logicalWidth / rect.width` is 1 in the
// unchanged case and the DPR never appears. Worked case: a 320x240 logical viewport at DPR 2 gives
// a 640x480 backing store and a 320x240 CSS box, so a click at CSS (10,10) is logical (10,10) - a
// `(clientX - rect.left) / dpr` would hand this function 5 and hit-test half a button away. The
// division by DPR is only correct if the page has set no CSS size at all and the canvas is being
// displayed at its natural (backing-store) size, which is not the layout this module is given.
//
// Unknown `kind` and a secondary `button` still answer a well-formed report (`dispatched` false,
// `changed` false), so a page typo costs no frame and no exception at the FFI boundary.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_pointer(const char* kind, float x, float y,
                                                        int button) {
    cli::CanvasPointerResult result;
    if (g_session == nullptr || !g_session->started()) {
        result.changed = false;
        return duplicate_out(cli::canvas_pointer_report_json(result));
    }
    bool known = false;
    const cli::PreviewInput::Kind parsed = cli::pointer_kind_from(
        kind != nullptr ? std::string_view(kind) : std::string_view(), known);
    if (!known) {
        // An unrecognized kind is a page bug, and dispatching it as a move would move the hover
        // somewhere the user never pointed.
        return duplicate_out(cli::canvas_pointer_report_json(result));
    }
    result = g_session->pointer(parsed, x, y, button > 0 ? static_cast<std::uint32_t>(button) : 0U);
    return duplicate_out(cli::canvas_pointer_report_json(result));
}

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
    if (g_session == nullptr || !g_session->started()) {
        return duplicate_out(cli::canvas_pointer_report_json(result));
    }
    const std::string_view name = kind != nullptr ? std::string_view(kind) : std::string_view();
    if (name == "blur") {
        return duplicate_out(cli::canvas_pointer_report_json(g_session->blur()));
    }
    bool known = false;
    const cli::PreviewInput::Kind parsed = cli::key_kind_from(name, known);
    const std::uint32_t code =
        cli::key_code_from_name(key != nullptr ? std::string_view(key) : std::string_view());
    if (!known || code == 0) {
        return duplicate_out(cli::canvas_pointer_report_json(result));
    }
    result = g_session->key(
        parsed, code, modifiers > 0 ? static_cast<std::uint32_t>(modifiers) : 0U, repeat != 0);
    return duplicate_out(cli::canvas_pointer_report_json(result));
}

// Changes the simulated fold posture of the RUNNING session (issue #408) and re-lays the tree out
// in place. Returns the same one-line report a Run returns, with `changed` set when a layout pass
// ran: the page draws one frame when it is true, and nothing when the posture was already in force.
//
// **Why a separate export rather than another `tessera_canvas_apply` argument.** A posture change
// must not re-evaluate the source: the state cells, the hover and the focus the router holds belong
// to the running evaluation, and a re-run would lose them. This is the export that keeps the
// session and changes one input of its layout - the device equivalent of turning a folding phone.
//
// `posture` is one of `flat`, `book`, `tabletop`; an empty string means "no fold", and an unknown
// name is reported rather than applied, because a control that looks like it worked while
// previewing an unfolded screen is the one failure this slice must not have.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_set_fold(const char* posture) {
    const auto extent = rhi::Extent2D{};
    if (g_session == nullptr || !g_session->started()) {
        cli::SceneFrameResult none;
        none.error = "no canvas session: call tessera_canvas_apply first";
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, false));
    }
    const std::string_view text =
        posture != nullptr ? std::string_view(posture) : std::string_view();
    tessera::hal::FoldPosture parsed = tessera::hal::FoldPosture::None;
    const std::string posture_error = cli::posture_from(text, parsed);
    if (!posture_error.empty()) {
        cli::SceneFrameResult none;
        none.error = posture_error;
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, false));
    }
    // `set_posture()` lays the tree out INSIDE this call, so the page's next `tessera_canvas_draw`
    // paints the new posture on its first frame - there is no "pending" state a later frame
    // applies, which is the issue's "no frame of stale padding" requirement made structural.
    const bool changed = g_session->set_posture(parsed);
    cli::SceneFrameResult report;
    // A report with no error and the layout counters zeroed: the page only needs `changed` and the
    // absence of `error`, and the frame counters come from the `tessera_canvas_draw` that follows.
    return duplicate_out(cli::canvas_fold_report_json(report, changed));
}

// Changes the simulated viewport of the RUNNING session (issue #436) and re-lays the tree out in
// place: `width`/`height` are the **logical** layout viewport (the space `tessera_canvas_pointer`
// takes its coordinates in) and `dpi` is the device pixel ratio, which reaches `@media
// (resolution)` as 96 dpi times it. No re-evaluation: the state cells, the hover and the focus
// belong to the running evaluation, and re-running would lose them - which is exactly what a
// device-preset switch used to do. It is the size equivalent of `tessera_canvas_set_fold`, and an
// orientation swap is this same call with the two sizes exchanged. The report's `cascadeGeneration`
// moves by one only when the size crosses an `@media` breakpoint (a resize is a layout pass
// otherwise), and `changed` says whether a frame is owed.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_set_viewport(int width, int height, float dpi) {
    const cli::CanvasViewportRequest request = cli::canvas_viewport_request(width, height, dpi);
    if (g_session == nullptr || !g_session->started() || !request.error.empty()) {
        const std::string error =
            request.error.empty()
                ? std::string("no canvas session: call tessera_canvas_apply first")
                : request.error;
        return duplicate_out(cli::canvas_viewport_report_json(false, request.width, request.height,
                                                              request.dpi, 0, error));
    }
    // The layout pass (and, when the size crosses a breakpoint, the cascade re-resolve) completes
    // INSIDE this call, so the page's next `tessera_canvas_draw` paints the new device on its first
    // frame - no pending state a later frame applies, and no frame of stale padding.
    const bool changed = g_session->set_viewport(request.width, request.height, request.dpi);
    return duplicate_out(cli::canvas_viewport_report_json(
        changed, request.width, request.height, request.dpi, g_session->cascade_generation(), {}));
}

// Changes the simulated skin's safe area of the RUNNING session (issue #171's skins slice) and
// re-lays the tree out in place: `inset_*` are the logical-pixel insets a device would report as
// `WindowMetrics::insets`, and `cutout_*` is the cutout rectangle (all zero for none).
//
// **Why this is its own export rather than more arguments on `tessera_canvas_set_viewport`.** The
// two answer different controls - the toolbar's size/DPR pair and the skin picker - and a page
// built against the previous module keeps calling the three-argument `set_viewport`, which is what
// "keep the existing arguments working" means. The engine side is one layout pass either way
// (`CanvasSceneSession::set_safe_area`), so the split costs nothing at runtime.
//
// **Why the engine has to hear about it at all.** `designer-studio-v1.md` section 5.1 says a skin's
// insets are "passed to the viewer as `safeAreas` in `viewport.set`, exactly as
// `WindowMetrics::insets` would arrive from the HAL", and that "the skin must never change the
// engine's logical size behind its back". A page-only overlay would draw a phone's status bar over
// a tree that was laid out as if the bar were not there - a skin that changes the picture without
// changing the layout. This export is what makes the `SafeArea` padding and every box follow the
// skin, through the same `hal::resolve_safe_area()` the native app runs.
//
// Returns the same one-line report a Run returns, with `changed` set when a layout pass ran: the
// page draws one frame when it is true, and nothing when the safe area was already in force.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_set_safe_area(float inset_left, float inset_top,
                                                              float inset_right, float inset_bottom,
                                                              float cutout_x, float cutout_y,
                                                              float cutout_width,
                                                              float cutout_height) {
    const auto extent = rhi::Extent2D{};
    const cli::CanvasSafeAreaRequest request =
        cli::canvas_safe_area_request(inset_left, inset_top, inset_right, inset_bottom, cutout_x,
                                      cutout_y, cutout_width, cutout_height);
    if (g_session == nullptr || !g_session->started() || !request.error.empty()) {
        cli::SceneFrameResult none;
        none.error = request.error.empty()
                         ? std::string("no canvas session: call tessera_canvas_apply first")
                         : request.error;
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, false));
    }
    // The layout pass completes INSIDE this call, so the page's next `tessera_canvas_draw` paints
    // the new safe area on its first frame - no frame of stale padding.
    const bool changed = g_session->set_safe_area(request.insets, request.cutouts);
    cli::SceneFrameResult report;
    return duplicate_out(cli::canvas_fold_report_json(report, changed));
}

// Draws the running session's current tree into the canvas texture view the page hands over, and
// returns the frame report. Called after `tessera_canvas_apply` and after any
// `tessera_canvas_pointer` that answered `changed: true`. Nothing here re-evaluates the source, so
// the picture is the state the pointer events produced; a caller that draws without a change
// repaints identical pixels.
//
// **It takes no strings** (issue #444): the tree it paints, and the author stylesheet that tree was
// laid out with (#390), are the ones the last `run`/`apply` left in the module. The report carries
// both the session's frame count and the sheet's diagnostics, so the page still learns about a bad
// CSS rule from the frame that paints its effect.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_draw(int width, int height, void* canvas_view) {
    const auto extent = rhi::Extent2D{static_cast<std::uint32_t>(width > 0 ? width : 0),
                                      static_cast<std::uint32_t>(height > 0 ? height : 0)};
    if (g_device == nullptr) {
        cli::SceneFrameResult none;
        none.error = "no WebGPU device: call tessera_webgpu_set_device first";
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, false));
    }
    if (g_session == nullptr || !g_session->started()) {
        cli::SceneFrameResult none;
        none.error = "no canvas session: call tessera_canvas_apply first";
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, true));
    }
    // **Paint first, into a named result.** `paint_session(...)` and `g_session->frames_painted()`
    // were two arguments of one call, and C++ leaves the order in which arguments are evaluated
    // unspecified. A build that reads the counter first reports the tally from *before* the paint,
    // so
    // `#stat-frame` shows `#1` forever while the session is visibly painting later frames.
    const cli::SceneFrameResult painted =
        cli::paint_session(*g_session, extent, canvas_view, *g_device);
    return duplicate_out(report_with_css(painted, extent, g_session->css_diagnostics(),
                                         g_session->frames_painted(),
                                         g_session->cascade_generation()));
}

// Releases the session (and with it the tree, its handlers and its carried state). The page calls
// it from **Restart app** and when it leaves the Canvas view; the next apply starts a fresh session
// with nothing carried, which is the same thing a page reload does.
//
// It returns a one-line JSON object like every other export in this module, and for the same
// reason: the page's `callExport` treats a null return as an error, and a void export read through
// it would surface as "_tessera_canvas_reset returned no document" on the first restart -- a
// crash-shaped message for a successful cold start. The document says what was released so the page
// can log it.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_reset() {
    const bool had_session = g_session != nullptr;
    const std::uint32_t frames = g_session != nullptr ? g_session->frames_painted() : 0;
    g_session.reset();
    std::string out = "{\"resetVersion\":1";
    out += ",\"released\":";
    out += had_session ? "true" : "false";
    out += ",\"frames\":";
    out += std::to_string(frames);
    out += "}\n";
    return duplicate_out(out);
}

// Loads the preview's one face from the bytes the page fetched from its own origin (issue #387).
// Returns 1 on success, 0 for empty data, -1 when the bytes are unusable or no device exists yet.
// The state, the replacement rule and the reason vocabulary live in `wasm_canvas_font.cpp`; this is
// only the C spelling of them.
EMSCRIPTEN_KEEPALIVE int tessera_canvas_set_font(const unsigned char* bytes, int size) {
    return cli::canvas_set_font(bytes, size, g_device.get());
}

// Whether a font is loaded and which face it is, as one JSON object:
//
//   {"fontVersion":1,"loaded":true,"family":"IBM Plex Sans","glyphsDropped":0,"atlasPages":1}
//
// `loaded:false` carries the reason in `error`, so the page has a named state to show rather than a
// canvas of empty boxes.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_font_status() {
    return duplicate_out(cli::canvas_font_status_json());
}

// The matching release, same convention as the diagnostics and scene modules.
EMSCRIPTEN_KEEPALIVE void tessera_free(const char* text) {
    std::free(const_cast<char*>(text));
}

} // extern "C"
