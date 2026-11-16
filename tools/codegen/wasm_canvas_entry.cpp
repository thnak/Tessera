// The browser entry point of the WebGPU canvas preview (issues #167, #388, #387, #408).
//
// Every export is specified in `docs/design/compiler-wasm.md` section 10, which is the normative
// source for the module ABI: `tessera_webgpu_set_device`, `tessera_canvas_frame`,
// `tessera_canvas_run`, `tessera_canvas_pointer`, `tessera_canvas_set_fold`, `tessera_canvas_draw`,
// `tessera_canvas_reset`, `tessera_canvas_set_font` and `tessera_canvas_font_status`. Read that
// before changing a signature; it also carries the "why Emscripten only" rationale and the
// verified-status record.
//
// The one rule that lives here rather than in the doc, because it is a property of this file: the
// exports below share one anonymous-namespace device and session, so they are not splittable into
// separate translation units without exposing that state.
//
// Notably: coordinates arriving from the page are **logical pixels**, and the CSS-to-logical rule
// is documented on `tessera_canvas_pointer` itself -- it is not "divide by devicePixelRatio".

#include "canvas_report.hpp"
#include "preview_text.hpp"
#include "scene_canvas_session.hpp"
#include "scene_frame.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tessera/rendering/webgpu/webgpu_device.hpp"
#include "wasm_canvas_font.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <emscripten/emscripten.h>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

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

// The canvas format the page configures. RGBA8 is one of the two formats WebGPU requires every
// canvas to support, and it is the format the paint path and the lavapipe goldens already use, so
// the preview paints the same bytes the reference does.
constexpr rhi::TextureFormat kCanvasFormat = rhi::TextureFormat::R8G8B8A8Unorm;

// Fills the canvas with the preview's clear colour and nothing else. A canvas texture is presented
// every frame whether or not anything drew into it, and an undrawn one shows as black, so a failed
// frame would otherwise replace the last picture with a black box that looks like a crash. Failure
// here is swallowed: the report already carries the frame's real error.
void clear_canvas(rhi::Device& device, const rhi::RenderTarget& target, rhi::Extent2D extent) {
    try {
        std::unique_ptr<rhi::CommandList> commands = device.create_command_list();
        if (commands == nullptr) {
            return;
        }
        rhi::RenderTargetDesc desc;
        desc.extent = extent;
        desc.format = kCanvasFormat;
        desc.clear_color = cli::scene_frame_clear_color();
        desc.load_op = rhi::LoadOp::Clear;
        desc.store_op = rhi::StoreOp::Store;
        commands->begin_render_pass(target, desc);
        commands->end_render_pass();
        commands->submit_async();
    } catch (...) {
    }
}

// Draws one frame into the canvas texture the page holds. The target takes ownership of the view
// first so that no later failure (a bad source) leaks the per-frame reference.
cli::SceneFrameResult paint_to_canvas(std::string_view source,
                                      const cli::ScenePreviewOptions& options,
                                      std::string_view component, rhi::Extent2D extent,
                                      void* canvas_view) {
    cli::SceneFrameResult result;
    if (extent.width == 0 || extent.height == 0) {
        result.error = "paint_to_canvas: the canvas extent must be positive";
        return result;
    }
    std::unique_ptr<rhi::RenderTarget> target;
    try {
        target = webgpu::create_canvas_target(canvas_view, extent, kCanvasFormat);
    } catch (const std::exception& failure) {
        result.error = std::string("paint_to_canvas: ") + failure.what();
        return result;
    }

    const cli::PreparedScene scene =
        cli::prepare_scene(source, options, component, cli::canvas_measurer());
    if (!scene.error.empty() || scene.root == nullptr) {
        result.error = scene.error.empty() ? "the source produced no scene" : scene.error;
        clear_canvas(*g_device, *target, extent);
        return result;
    }

    rhi::RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = kCanvasFormat;
    desc.clear_color = cli::scene_frame_clear_color();
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    cli::SceneFrameResult painted =
        cli::scene_frame_paint(scene, *g_device, *target, desc, {}, cli::canvas_text_stack());
    if (!painted.error.empty()) {
        clear_canvas(*g_device, *target, extent);
    }
    return painted;
}

// The persistent session (#388): one evaluation the pointer events feed and every later frame
// paints. It is created by `tessera_canvas_run` and replaced by the next Run, which carries the
// state cells across the edit (`scene_canvas_session.hpp`). Declared here rather than beside
// `g_device` because it must die before any device it painted through.
std::unique_ptr<cli::CanvasSceneSession> g_session;

// The normalized pointer kinds the page may send. Anything else is reported as an error instead of
// being coerced to a move: a typo in the page would otherwise silently paint the wrong state.
cli::PreviewInput::Kind pointer_kind_from(std::string_view name, bool& known) {
    known = true;
    if (name == "move") {
        return cli::PreviewInput::Kind::PointerMove;
    }
    if (name == "down") {
        return cli::PreviewInput::Kind::PointerDown;
    }
    if (name == "up") {
        return cli::PreviewInput::Kind::PointerUp;
    }
    if (name == "cancel") {
        return cli::PreviewInput::Kind::PointerCancel;
    }
    known = false;
    return cli::PreviewInput::Kind::PointerMove;
}

// Paints the session's tree into the canvas texture the page holds. Unlike `paint_to_canvas` this
// never re-evaluates: the tree is the one the pointer events have been dispatched into, so its
// hover/active bits and its state are the ones the frame shows.
cli::SceneFrameResult paint_session_to_canvas(rhi::Extent2D extent, void* canvas_view) {
    cli::SceneFrameResult result;
    if (extent.width == 0 || extent.height == 0) {
        result.error = "paint_session_to_canvas: the canvas extent must be positive";
        return result;
    }
    rhi::RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = kCanvasFormat;
    desc.clear_color = cli::scene_frame_clear_color();
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;

    std::unique_ptr<rhi::RenderTarget> target;
    try {
        target = canvas_view != nullptr
                     ? webgpu::create_canvas_target(canvas_view, extent, kCanvasFormat)
                     : g_device->create_render_target(desc);
    } catch (const std::exception& failure) {
        result.error = std::string("paint_session_to_canvas: ") + failure.what();
        return result;
    }
    if (target == nullptr) {
        result.error = "paint_session_to_canvas: no render target";
        return result;
    }
    result = g_session->paint(*g_device, *target, desc, {}, cli::canvas_text_stack());
    if (!result.error.empty() && canvas_view != nullptr) {
        clear_canvas(*g_device, *target, extent);
    }
    return result;
}

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
EMSCRIPTEN_KEEPALIVE const char* tessera_canvas_frame(const char* source, const char* file,
                                                      const char* component,
                                                      const char* fold_posture, int width,
                                                      int height, float dpi, void* canvas_view) {
    // Two different rectangles, and the whole point of this block is not to confuse them:
    //   * `width` x `height` are the LOGICAL layout viewport (the page's CSS box), which is what
    //   the
    //     layout engine and `WindowMetrics` take;
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
    if (!posture_text.empty()) {
        const std::optional<tessera::hal::FoldPosture> parsed =
            tessera::hal::parse_fold_posture(posture_text);
        if (!parsed) {
            cli::SceneFrameResult none;
            none.error = "TESSERA_STUDIO_FOLD_POSTURE: `" + std::string(posture_text) +
                         "` is not a fold posture; expected flat, book or tabletop";
            return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, true));
        }
        posture = *parsed;
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
        return duplicate_out(cli::canvas_report_json(result, extent.width, extent.height, true));
    }
    return duplicate_out(
        cli::canvas_report_json(paint_to_canvas(source, options, name, extent, canvas_view),
                                extent.width, extent.height, true));
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
                                                    int width, int height, float dpi) {
    const auto extent = rhi::Extent2D{static_cast<std::uint32_t>(width > 0 ? width : 0),
                                      static_cast<std::uint32_t>(height > 0 ? height : 0)};
    if (g_device == nullptr) {
        cli::SceneFrameResult none;
        none.error = "no WebGPU device: call tessera_webgpu_set_device first";
        return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, false));
    }
    // The posture is validated before anything is built: a typo must not silently start the session
    // unfolded, because the page would then show a plausible picture of the wrong device.
    tessera::hal::FoldPosture posture = tessera::hal::FoldPosture::None;
    const std::string_view posture_text =
        fold_posture != nullptr ? std::string_view(fold_posture) : std::string_view();
    if (!posture_text.empty()) {
        const std::optional<tessera::hal::FoldPosture> parsed =
            tessera::hal::parse_fold_posture(posture_text);
        if (!parsed) {
            cli::SceneFrameResult none;
            none.error = "TESSERA_STUDIO_FOLD_POSTURE: `" + std::string(posture_text) +
                         "` is not a fold posture; expected flat, book or tabletop";
            return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, true));
        }
        posture = *parsed;
    }
    if (g_session == nullptr) {
        g_session = std::make_unique<cli::CanvasSceneSession>();
    }
    cli::CanvasSessionSource options;
    options.text = source != nullptr ? std::string_view(source) : std::string_view();
    options.file = file != nullptr ? std::string_view(file) : std::string_view();
    options.component = component != nullptr ? std::string_view(component) : std::string_view();
    // The page's salt is the project namespace it shows; the same constant the scene module uses so
    // the two views name one node by one number.
    options.salt = "preview";
    options.width = static_cast<float>(extent.width);
    options.height = static_cast<float>(extent.height);
    options.dpi_scale = dpi > 0.0f ? dpi : 1.0f;
    options.posture = posture;
    // The session measures its text with the same face every frame paints with, so a line never
    // overflows or under-fills the box it was laid out in (#387).
    options.measurer = cli::canvas_measurer();

    const std::string error = g_session->start(options);
    cli::SceneFrameResult report;
    report.error = error;
    return duplicate_out(cli::canvas_report_json(report, extent.width, extent.height, true));
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
    const cli::PreviewInput::Kind parsed =
        pointer_kind_from(kind != nullptr ? std::string_view(kind) : std::string_view(), known);
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
    if (!text.empty()) {
        const std::optional<tessera::hal::FoldPosture> known =
            tessera::hal::parse_fold_posture(text);
        if (!known) {
            cli::SceneFrameResult none;
            none.error = "TESSERA_STUDIO_FOLD_POSTURE: `" + std::string(text) +
                         "` is not a fold posture; expected flat, book or tabletop";
            return duplicate_out(cli::canvas_report_json(none, extent.width, extent.height, false));
        }
        parsed = *known;
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
    return duplicate_out(cli::canvas_report_json(paint_session_to_canvas(extent, canvas_view),
                                                 extent.width, extent.height, true));
}

// Releases the session (and with it the tree, its handlers and its carried state). The page calls
// it when it leaves the Canvas view; a later Run starts a fresh session with no carried state,
// which is the same thing a page reload does.
EMSCRIPTEN_KEEPALIVE void tessera_canvas_reset() {
    g_session.reset();
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
