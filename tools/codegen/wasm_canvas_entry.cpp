// The browser entry point of the WebGPU canvas preview (issues #167, #388, #387, #408, #444).
//
// Every export is specified in `docs/design/compiler-wasm.md` section 10, which is the normative
// source for the module ABI: `tessera_webgpu_set_device`, `tessera_canvas_frame`,
// `tessera_canvas_run`, `tessera_canvas_apply`, `tessera_canvas_pointer`,
// `tessera_canvas_set_fold`, `tessera_canvas_draw`, `tessera_canvas_reset`,
// `tessera_canvas_set_font` and `tessera_canvas_font_status`. Read that before changing a
// signature; it also carries the "why Emscripten only" rationale and the verified-status record.
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
#include "wasm_canvas_support.hpp"

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

// The frame report for one painted frame, with the author stylesheet's problems beside its counters
// (issue #390). One helper because the offscreen and canvas paths must report one document shape.
//
// `frames` is the session's lifetime count where there is one (#444) and 0 on the stateless
// `tessera_canvas_frame` path, which has no session to count; a frame report is the same document
// whether or not the caller has one, so the two features append to one serializer rather than
// producing two report shapes.
std::string frame_report(const cli::SceneFrameResult& result,
                         const cli::ScenePreviewOptions& options, rhi::Extent2D extent,
                         std::uint32_t frames = 0) {
    return cli::canvas_report_json(
        result, extent.width, extent.height, true, frames,
        cli::author_stylesheet_diagnostics(options.stylesheet, options.stylesheet_file));
}

// The same report with a session's already-collected diagnostics (a Run, an Apply or a Draw does
// not re-prepare the sheet, so it reports what `start()` found) and the session's own frame count.
std::string report_with_css(const cli::SceneFrameResult& result, rhi::Extent2D extent,
                            const std::vector<cli::PreviewCssDiagnostic>& css,
                            std::uint32_t frames = 0) {
    return cli::canvas_report_json(result, extent.width, extent.height, true, frames, css);
}

// The persistent session (#388): one evaluation the pointer events feed and every later frame
// paints. It is created by `tessera_canvas_run` and replaced by the next Run, which carries the
// state cells across the edit (`scene_canvas_session.hpp`). Declared here rather than beside
// `g_device` because it must die before any device it painted through.
std::unique_ptr<cli::CanvasSceneSession> g_session;

} // namespace

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

// `canvas_view` is the Emscripten handle of the canvas's current texture view
// (`WebGPU.importJsTextureView(context.getCurrentTexture().createView())`), one reference per
// frame, which the frame takes ownership of. Null draws into an offscreen target instead (the Node
// probe and any caller with no canvas), so the counters stay available without a surface.
//
// `stylesheet` is the author's CSS (issue #390), the page's second textarea. Null or empty means
// "no author CSS", which is what every caller written before #390 passes and what keeps the
// built-in `preview_stylesheet()` fallback in force -- so the Node probe and an older page keep
// working unchanged. A non-empty sheet REPLACES the built-in one (decision 2) and never mixes with
// it.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_frame(const char* source, const char* file,
                                                      const char* component,
                                                      const char* fold_posture,
                                                      const char* stylesheet, int width, int height,
                                                      float dpi, void* canvas_view) {
    // Two different rectangles, and the whole point of this block is not to confuse them:
    //   * `width` x `height` are the LOGICAL layout viewport (the page's CSS box), which is what
    //     the layout engine and `WindowMetrics` take;
    //   * `extent` is the device-pixel render target, `logical * dpi`, which is what the GPU draws
    //     into. Deriving it here rather than trusting a second pair of arguments keeps one source
    //     of truth (logical size plus DPR) on the page side.
    const float logical_width = width > 0 ? static_cast<float>(width) : 320.0f;
    const float logical_height = height > 0 ? static_cast<float>(height) : 240.0f;
    const float scale = dpi > 0.0f ? dpi : 1.0f;
    const auto extent =
        rhi::Extent2D{static_cast<std::uint32_t>(std::lround(logical_width * scale)),
                      static_cast<std::uint32_t>(std::lround(logical_height * scale))};
    if (g_device == nullptr) {
        cli::SceneFrameResult none;
        none.error = "no WebGPU device: call tessera_webgpu_set_device first";
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, false));
    }
    // The posture is validated before anything is built: a typo must not silently lay out an
    // unfolded device, because the page would then show a plausible picture of the wrong thing.
    tessera::hal::FoldPosture posture = tessera::hal::FoldPosture::None;
    const std::string_view posture_text =
        fold_posture != nullptr ? std::string_view(fold_posture) : std::string_view();
    const std::string posture_error = cli::posture_from(posture_text, posture);
    if (!posture_error.empty()) {
        cli::SceneFrameResult none;
        none.error = posture_error;
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, true));
    }

    cli::ScenePreviewOptions options;
    if (file != nullptr && file[0] != '\0') {
        options.file = file;
    }
    // The layout rectangle is the LOGICAL viewport, which is what the page's CSS box is; `dpi` only
    // scales the backing store computed above and must never move a layout coordinate. (This used
    // to hard-code 320x240 here while the page sized its backing store from the toolbar, so a
    // non-default viewport drew a 320x240 layout scaled into a larger canvas.)
    options.width = logical_width;
    options.height = logical_height;
    options.dpi_scale = scale;
    options.salt = "preview";
    options.posture = posture;
    // The author stylesheet, when the page sent one. Its absence is not an error: an old page, the
    // Node probe and a visitor who cleared the second textarea all land on the built-in fallback.
    options.stylesheet = stylesheet != nullptr ? std::string_view(stylesheet) : std::string_view();
    const std::string_view name =
        component != nullptr ? std::string_view(component) : std::string_view();
    if (source == nullptr) {
        cli::SceneFrameResult none;
        none.error = "no source";
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, true));
    }

    if (canvas_view == nullptr) {
        const cli::SceneFrameResult result = cli::scene_frame_render(
            source, options, *g_device, extent, name, {}, cli::canvas_text_stack());
        // The offscreen path still reports the author sheet's problems, so the Node probe and the
        // no-canvas caller see the same diagnostics the canvas does (issue #390).
        return duplicate_out(frame_report(result, options, extent));
    }
    return duplicate_out(
        frame_report(cli::paint_to_canvas(source, options, name, extent, canvas_view, *g_device),
                     options, extent));
}

// Starts (or restarts) the persistent session: one evaluation that later `tessera_canvas_pointer`
// calls feed and later `tessera_canvas_draw` calls paint. Returns the same one-line JSON report a
// frame returns, with `error` set when the source produced no scene; the page shows the previous
// picture and the reason rather than a blank canvas.
//
// `width`/`height` are the **logical** layout viewport, which is also the coordinate space of
// `tessera_canvas_pointer`. See that function for the CSS-to-logical mapping; `dpi` is only what
// scales the backing store and never moves a hit-test coordinate.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_run(const char* source, const char* file,
                                                    const char* component, const char* fold_posture,
                                                    const char* stylesheet, int width, int height,
                                                    float dpi) {
    const auto extent = rhi::Extent2D{static_cast<std::uint32_t>(width > 0 ? width : 0),
                                      static_cast<std::uint32_t>(height > 0 ? height : 0)};
    if (g_device == nullptr) {
        cli::SceneFrameResult none;
        none.error = "no WebGPU device: call tessera_webgpu_set_device first";
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, false));
    }
    if (g_session == nullptr) {
        g_session = std::make_unique<cli::CanvasSceneSession>();
    }
    cli::CanvasSessionSource options;
    const std::string request_error = cli::canvas_session_source(
        source != nullptr ? std::string_view(source) : std::string_view(),
        file != nullptr ? std::string_view(file) : std::string_view(),
        component != nullptr ? std::string_view(component) : std::string_view(),
        fold_posture != nullptr ? std::string_view(fold_posture) : std::string_view(),
        // The author stylesheet (issue #390). Null/empty is "no author CSS": the built-in fallback
        // stays, which is what an older page and a cleared textarea both mean.
        stylesheet != nullptr ? std::string_view(stylesheet) : std::string_view(),
        static_cast<float>(extent.width), static_cast<float>(extent.height), dpi, options);
    if (!request_error.empty()) {
        cli::SceneFrameResult none;
        none.error = request_error;
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, true));
    }

    const std::string error = g_session->start(options);
    cli::SceneFrameResult report;
    report.error = error;
    // A Run reports the author sheet's problems even when the source itself produced no scene: the
    // page shows one diagnostics area, so a visitor who fixed the TSX need not Restart twice to
    // learn the stylesheet was also wrong.
    return duplicate_out(report_with_css(report, extent, g_session->css_diagnostics()));
}

// Applies an edit to the RUNNING session (issue #444: the page's live edit), without resetting it.
//
// This is `tessera_canvas_run` with two differences that the live edit needs and a button does not:
//
//  1. **It reports `changed`.** `run` answers "was the source evaluated", because the page then
//  draws
//     unconditionally; an auto-apply of text that did not change must cost no frame, so this one
//     answers the session's own `needs_frame()` and the page draws only when it is true. A `reused`
//     apply (identical text) is therefore `changed:false` and zero frames.
//  2. **It reports the carry split.** `carried`/`droppedComponent`/`droppedCell`/`droppedType` come
//     from `StateCarryReport` (`scene_state.hpp`), so the page can say which cells survived the
//     edit and, when a component was removed, that the spec's reset case happened rather than a
//     silent loss.
//
// A source that does not evaluate leaves the running evaluation and its picture untouched (see
// `CanvasSceneSession::start`), and this returns `error` with `changed` as it already was: the page
// shows the diagnostic and draws nothing, which is what keeps the canvas from blanking
// mid-keystroke.
//
// `width`/`height`/`dpi` are the same logical viewport and DPR `tessera_canvas_run` takes, and are
// part of the tuple that decides reuse: a toolbar change is an apply too (one frame), and a font
// that arrived after the last evaluation re-lays out.
//
// `stylesheet` is the author's CSS (issue #390) and travels **here**, on the apply, for exactly the
// reason it travels on `run`: an apply is the call that re-evaluates, so it is the call that
// decides which sheet the tree is laid out with. `tessera_canvas_draw` takes no strings at all --
// it paints the tree the last apply left -- so a sheet handed to `draw` would have nowhere to go.
// That is the whole shape of the live edit with an author stylesheet: the edit re-applies the
// sheet, and the frame that follows keeps the author's look.
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_apply(const char* source, const char* file,
                                                      const char* component,
                                                      const char* fold_posture,
                                                      const char* stylesheet, int width, int height,
                                                      float dpi) {
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
    cli::CanvasSessionSource options;
    const std::string request_error = cli::canvas_session_source(
        source != nullptr ? std::string_view(source) : std::string_view(),
        file != nullptr ? std::string_view(file) : std::string_view(),
        component != nullptr ? std::string_view(component) : std::string_view(),
        fold_posture != nullptr ? std::string_view(fold_posture) : std::string_view(),
        stylesheet != nullptr ? std::string_view(stylesheet) : std::string_view(),
        static_cast<float>(extent.width > 0 ? extent.width : 0),
        static_cast<float>(extent.height > 0 ? extent.height : 0), dpi, options);
    if (!request_error.empty()) {
        cli::SceneFrameResult none;
        none.error = request_error;
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, true));
    }

    // The posture is not part of the tuple `start()` compares to decide reuse (it is a layout
    // input, not a source), so a reused apply must still hand the new one to `set_posture()`. That
    // is the #408 path: the running tree is re-laid out in place, keeping hover, focus and the
    // state cells, and reporting whether a layout pass ran - which is what makes this frame the
    // first to show the new posture. On a fresh evaluation `start()` already laid the tree out at
    // this posture, and `set_posture()` answers false, so this is one call in both cases and never
    // a second pass.
    const std::string error = g_session->start(options);
    if (error.empty() && g_session->reused()) {
        (void)g_session->set_posture(options.posture);
    }
    return duplicate_out(cli::canvas_apply_report_json(
        g_session->needs_frame(), g_session->reused(), g_session->carry_report(),
        g_session->state(), g_session->frames_painted(), error));
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

// Changes the simulated fold posture of the RUNNING session (issue #408) and re-lays the tree out
// in place. Returns the same one-line report a Run returns, with `changed` set when a layout pass
// ran: the page draws one frame when it is true, and nothing when the posture was already in force.
//
// **Why a separate export rather than another `tessera_canvas_run` argument.** A posture change
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
        none.error = "no canvas session: call tessera_canvas_run first";
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

// Draws the running session's current tree into the canvas texture view the page hands over, and
// returns the frame report. Called after `tessera_canvas_run` and after any
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
        none.error = "no canvas session: call tessera_canvas_run first";
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, true));
    }
    return duplicate_out(
        report_with_css(cli::paint_session(*g_session, extent, canvas_view, *g_device), extent,
                        g_session->css_diagnostics(), g_session->frames_painted()));
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
