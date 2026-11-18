// SPDX-License-Identifier: MIT

#include "diagnostic_record.hpp"
#include "scene_canvas_session.hpp"
#include "tessera/ui/dirty.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::compiler::codegen::cli {
namespace {

namespace ui = tessera::ui;
namespace hal = tessera::hal;

// The domains a frame owes. Style is what `:hover`/`:active` sets through
// `Node::set_pseudo_state`; Layout/Paint/Render are what the post-dispatch rebuild sets. Any of
// them means the painted pixels could differ from the last frame's.
constexpr ui::Dirty kFrameDomains =
    ui::Dirty::Style | ui::Dirty::Layout | ui::Dirty::Paint | ui::Dirty::Render;

} // namespace

CanvasSceneSession::CanvasSceneSession()
    : sheet_(compile_preview_stylesheet({}, preview_media_context(320.0f, 240.0f, 1.0f))) {}

CanvasSceneSession::~CanvasSceneSession() = default;

std::string CanvasSceneSession::start(const CanvasSessionSource& source) {
    const std::string text(source.text);
    const std::string stylesheet(source.stylesheet);
    const std::string file = normalize_path(source.file.empty() ? "ui/Preview.tsx" : source.file);
    const std::string component(source.component);
    const float dpi = source.dpi_scale > 0.0f ? source.dpi_scale : 1.0f;

    // A Run of the identical source is a repaint, not a re-evaluation. The page hands the editor's
    // text to every frame, so re-evaluating here would drop the hover bits and the state cells
    // between two frames of the same program. The stylesheet is part of the identity: a Run that
    // changed only the CSS must re-resolve style, or the canvas would keep painting the previous
    // sheet while the diagnostics area reported the new one.
    //
    // **The viewport and the posture are NOT part of it** (issue #436). They are layout inputs, so
    // a Run that only moved the device preset re-lays the RUNNING tree out in place - one pass, no
    // evaluation - which is what keeps hover/focus/active and the state cells across a device or
    // orientation change. Before this, width/height were in `reused_ok`, so a toolbar change
    // re-evaluated the source and lost exactly that.
    if (session_ != nullptr && reused_ok(text, stylesheet, file, component, source.measurer)) {
        reused_ = true;
        (void)apply_layout_inputs(source.width, source.height, dpi, source.posture);
        return {};
    }

    // **The new source is evaluated before the running evaluation is touched.** That ordering is
    // what makes a *live* edit safe (issue #444): a developer typing in the editor passes through
    // sources that do not parse (`if (count > 0) {` with no closing brace yet), and each of those
    // must cost a diagnostic only - never the running tree, its state cells, the sheet its boxes
    // were laid out with, or the last painted frame. A failed evaluation therefore leaves
    // `session_`, `evaluated_`, `sheet_`, `css_diagnostics_` and the recorded source tuple exactly
    // as they were, so the session still owes no frame (`needs_frame()` is false), the page paints
    // nothing, and the pixels already on the canvas stay; the next source that does evaluate
    // captures the state from the tree that survived, which is why a counter typed through a
    // transient syntax error still holds its count.
    //
    // The author sheet (issue #390) is compiled *after* the evaluation succeeds, not before: it and
    // the tree it styles have to move together, because a sheet swapped in over a tree laid out for
    // the previous one would paint the old boxes through the new cascade. Nothing is lost from
    // #390 by that ordering -- the moment the TSX parses, the CSS diagnostics the author typed are
    // reported with the frame that paints them, which is #390's acceptance case. A CSS problem
    // typed while the TSX is mid-word is reported as soon as the source evaluates again, and a
    // developer never has to press Restart to find out.
    const std::string_view stylesheet_file = source.stylesheet_file;
    auto evaluated =
        evaluate_source(text, file, source.salt, component, source.stylesheet, stylesheet_file);
    if (!evaluated) {
        error_ = evaluated.error().detail;
        if (error_.empty()) {
            error_ = "the source produced no component";
        }
        reused_ = false;
        // `fresh_` is deliberately NOT cleared: a frame a previous successful edit already owed is
        // still owed, and clearing it here would leave that edit's tree unpainted forever.
        return error_;
    }

    // Capture before the old tree dies: `capture()` reads the scope the tree was built from, and
    // the tree borrows strings from `evaluated_`, which is about to be replaced.
    CarriedState carried;
    if (session_ != nullptr) {
        carried = session_->capture();
    }
    session_.reset();
    error_.clear();
    reused_ = false;
    fresh_ = false;
    // The sheet and its diagnostics, from the same text and in that order: the diagnostics describe
    // the sheet the tree below is laid out with, so the pane can never report one sheet's problems
    // beside another sheet's pixels.
    //
    // The sheet is PREPARED once and resolved against the viewport (issue #436), and the prepared
    // form goes into the session so a later viewport change can re-resolve the same data instead of
    // re-parsing CSS.
    const ui::style::MediaContext media = preview_media_context(source.width, source.height, dpi);
    // A `*.module.css` is scoped before it is compiled (issue #463), so the class names the tree
    // carries match the rules. The same text feeds the diagnostics below, so the pane can never
    // report one sheet's problems beside another sheet's pixels.
    std::optional<css_module::ModuleScanError> scan_error;
    const std::string stylesheet_text =
        preview_stylesheet_text(source.stylesheet, stylesheet_file, scan_error);

    // The prepared form goes into the session so a later viewport change can re-resolve the same
    // data instead of re-parsing CSS (issue #436).
    std::optional<ui::style::PreparedStyleSheet> prepared;
    if (scan_error) {
        // Nothing from a sheet that cannot be scoped is compiled: painting its raw `.card` rules
        // while the tree says `card_0b82a5` is the silent-unstyled defect #463 closes. The sheet is
        // an EMPTY one, not the built-in fallback - the author did write CSS, and #390 decision 2
        // replaces the built-in sheet with the author's rather than resurrecting it for a broken
        // one.
        sheet_ =
            resolve_preview_stylesheet(ui::style::prepare_stylesheet(std::string_view{}), media);
    } else {
        prepared = prepare_preview_stylesheet(stylesheet_text);
        sheet_ = resolve_preview_stylesheet(*prepared, media);
    }
    css_diagnostics_ = author_stylesheet_diagnostics(stylesheet_text, stylesheet_file);
    if (scan_error) {
        // The scan's offset indexes the AUTHOR's text, which is the file the author is looking at.
        const std::string_view sheet_file =
            stylesheet_file.empty() ? kPreviewStylesheetFile : stylesheet_file;
        const DiagnosticSpan span = css_span(sheet_file, source.stylesheet, scan_error->offset);
        PreviewCssDiagnostic diagnostic;
        diagnostic.code = scan_error->code;
        diagnostic.message = scan_error->message;
        diagnostic.file = std::string(sheet_file);
        diagnostic.offset = span.offset;
        diagnostic.line = span.line;
        diagnostic.column = span.column;
        // An error, not a warning: unlike an unknown property, this one costs the whole sheet.
        diagnostic.warning = false;
        css_diagnostics_.push_back(std::move(diagnostic));
    }
    evaluated_ = std::move(*evaluated);

    // Every component id in the new evaluation, so a carried cell whose component disappeared is
    // reported as dropped instead of being offered to whatever component took its place.
    std::vector<std::string> live_ids;
    live_ids.reserve(evaluated_.module.components.size());
    for (const auto& candidate : evaluated_.module.components) {
        live_ids.push_back(candidate.component_id);
    }

    PreviewSession::Options options;
    options.width = source.width;
    options.height = source.height;
    options.dpi_scale = dpi;
    options.posture = source.posture;
    options.carried = &carried;
    options.live_component_ids = &live_ids;
    options.measurer = source.measurer;
    options.prepared = std::move(prepared);
    options.media = media;

    session_ = std::make_unique<PreviewSession>(evaluated_.module, evaluated_.selected(), sheet_,
                                                std::move(options));
    if (!session_->built()) {
        error_ = "TESSERA_SCENE_UNSUPPORTED: the source's tree cannot be previewed";
        return error_;
    }
    carry_report_ = session_->carry_report();
    metrics_ = session_->metrics();
    text_ = text;
    stylesheet_ = stylesheet;
    file_ = file;
    component_ = component;
    width_ = source.width;
    height_ = source.height;
    dpi_scale_ = dpi;
    posture_ = source.posture;
    measurer_ = source.measurer;
    fresh_ = true;
    return error_;
}

bool CanvasSceneSession::reused_ok(
    const std::string& text, const std::string& stylesheet, const std::string& file,
    const std::string& component, const std::shared_ptr<const text::TextMeasurer>& measurer) const {
    // The measurer is part of the identity: a Run that arrives with font bytes when the running
    // evaluation had none must re-evaluate, because every text box it laid out was sized by the
    // font-free stub.
    //
    // The viewport and the DPI scale are deliberately NOT here (issue #436): they are layout inputs
    // the running tree can be re-laid out for, not properties of the source. Keeping them in this
    // tuple is what made a device-preset change re-evaluate - and lose hover, focus and active.
    return text == text_ && stylesheet == stylesheet_ && file == file_ && component == component_ &&
           measurer.get() == measurer_.get();
}

bool CanvasSceneSession::set_posture(hal::FoldPosture posture) {
    return apply_layout_inputs(width_, height_, dpi_scale_, posture);
}

bool CanvasSceneSession::set_viewport(float width, float height, float dpi_scale) {
    return apply_layout_inputs(width, height, dpi_scale, posture_);
}

bool CanvasSceneSession::apply_layout_inputs(float width, float height, float dpi_scale,
                                             hal::FoldPosture posture) {
    if (!started()) {
        return false;
    }
    const float dpi = dpi_scale > 0.0f ? dpi_scale : 1.0f;
    if (width == width_ && height == height_ && dpi == dpi_scale_ && posture == posture_) {
        return false;
    }
    // One pass for both inputs, inside this call: the boxes (and, when the size crosses a
    // breakpoint, the cascade) are already the new ones when it returns, so the frame the page
    // draws next is the FIRST to show the new device - no frame of stale padding or stale
    // breakpoint (issues #408 and #436).
    if (!session_->relayout_at(width, height, dpi, posture)) {
        return false;
    }
    width_ = width;
    height_ = height;
    dpi_scale_ = dpi;
    posture_ = posture;
    metrics_ = session_->metrics();
    return true;
}

CanvasPointerResult CanvasSceneSession::pointer(PreviewInput::Kind kind, float x, float y,
                                                std::uint32_t button) {
    CanvasPointerResult result;
    if (!started()) {
        return result;
    }
    // This slice wires the primary button only: a secondary press has no `CanvasAction` yet, so
    // dispatching it as a left press would run an `onClick` the user never asked for. A move
    // carries no button and is always dispatched.
    const bool contact =
        kind == PreviewInput::Kind::PointerDown || kind == PreviewInput::Kind::PointerUp;
    if (contact && button != 0) {
        return result;
    }

    PreviewInput input;
    input.kind = kind;
    input.x = x;
    input.y = y;
    result.dispatched = session_->dispatch(input);
    result.ran = session_->last_ran();
    result.unrun = session_->last_unrun();

    if (const ui::Node* hover = session_->router().hovered()) {
        result.hovered_id = hover->automation_id();
    }
    if (const ui::Node* active = session_->router().active()) {
        result.active_id = active->automation_id();
    }
    result.target_id = result.dispatched ? result.hovered_id : 0;
    // "A frame is owed" is the scene's own answer, never a guess: a dispatch that changed no
    // style/layout/paint domain costs no frame.
    result.changed = needs_frame();
    return result;
}

bool CanvasSceneSession::needs_frame() const {
    if (!started()) {
        return false;
    }
    if (fresh_) {
        return true;
    }
    return ui::has_any(session_->root().subtree_dirty(), kFrameDomains);
}

SceneFrameResult CanvasSceneSession::paint(rhi::Device& device, rhi::RenderTarget& target,
                                           const rhi::RenderTargetDesc& desc,
                                           const rhi::PipelineDesc& pipeline,
                                           PreviewTextStack* text) {
    SceneFrameResult result;
    if (!started()) {
        result.error = error_.empty() ? "the session has no scene to paint" : error_;
        return result;
    }
    result =
        scene_frame_paint_tree(session_->root(), sheet(), device, target, desc, pipeline, text);
    if (!result.error.empty()) {
        return result;
    }
    // The frame consumed the dirty domains. A static scene therefore has none set, which is what
    // makes `needs_frame()` answer false and the caller draw nothing until the next event.
    dirty_.clear(session_->root(), kFrameDomains);
    fresh_ = false;
    ++frames_painted_;
    return result;
}

const ui::Node* CanvasSceneSession::root() const noexcept {
    return session_ != nullptr ? &session_->root() : nullptr;
}

const ui::InputRouter* CanvasSceneSession::router() const noexcept {
    return session_ != nullptr ? &session_->router() : nullptr;
}

} // namespace tessera::compiler::codegen::cli
