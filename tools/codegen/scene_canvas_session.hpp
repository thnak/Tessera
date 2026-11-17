// SPDX-License-Identifier: MIT

#pragma once

#include "scene_evaluate.hpp"
#include "scene_frame.hpp"
#include "scene_prepare.hpp"
#include "scene_session.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The canvas module's persistent session (issue #388, P3c): one evaluation that pointer events feed
// and that a frame paints, kept alive between events.
//
// **Why it cannot be `prepare_scene()` per call.** The scene view's `tessera_input_json` evaluates
// from source on every call, applies a script and returns the state cells; the state it reached is
// gone when the call returns, because it belonged to a tree that call owned. A canvas needs the
// opposite: the tree, its `ui::InputRouter` (hover/active/focus/capture) and its state scope must
// outlive one pointer event and be visible to the *next* frame. That is the whole of this file.
//
// **What the pointer path is.** An event goes through `PreviewSession::dispatch`, which is the
// production `ui::InputRouter` (`src/ui/scene/input_dispatch.cpp`): hit testing, implicit pointer
// capture, click synthesis on the nearest common ancestor, and capture/target/bubble propagation.
// The router sets `PseudoState::Hover` on the hit node and `PseudoState::Active` on the press
// target and its ancestors, and `style::SceneStyleTree` refreshes each node's selector view from
// `Node::pseudo_state()` as it computes, so `:hover`/`:active` match through the ordinary cascade.
// Nothing in this file parses a selector or matches a pseudo-class.
//
// **Why `changed` and not "always redraw".** The repository's idle rule is that a static UI
// presents nothing (AGENTS.md; docs/design/dirty-rendering.md), and the canvas page honours it with
// one on-demand frame per change. This session answers "is a frame owed" from the scene's own dirty
// domains - the bits `Node::set_pseudo_state` and the post-dispatch rebuild already set - so an
// event that changed nothing (a move that stays over the same node, a move outside the scene whose
// hover was already cleared) requests no frame at all.
namespace tessera::compiler::codegen::cli {

// The text module this session's font parameter names lives one namespace up.
namespace text = tessera::ui::text;

// What one pointer event did. `changed` is the page's answer to "schedule one frame?".
struct CanvasPointerResult {
    bool dispatched = false; // the router delivered the event to a scene target
    bool changed = false;    // a style/layout/paint domain moved, so a frame is owed
    std::uint64_t target_id = 0;
    // The nodes carrying the two pseudo-classes after the event, so a caller can name what it sees
    // painted without walking the tree. 0 when that state is absent.
    std::uint64_t hovered_id = 0;
    std::uint64_t active_id = 0;
    std::vector<std::string> ran;   // IR handler ids whose preview body executed
    std::vector<std::string> unrun; // handler ids the dispatch reached but could not run
};

// What a session is started from. `width`/`height` are the **logical** viewport the layout engine
// sizes the tree to, which is also the coordinate space pointer events arrive in: the page derives
// them from its CSS box and never hands this layer a device pixel ratio.
//
// **The viewport and the posture are layout inputs, not part of the source's identity** (issue
// #436). They are on this struct because a `start()` has to lay a FRESH evaluation out at the right
// rectangle, but `start()` does not compare them when it decides to reuse a running evaluation:
// `set_viewport()` and `set_posture()` re-lay the running tree out in place instead, which is what
// keeps hover, focus and the state cells across a device, orientation or posture change.
struct CanvasSessionSource {
    std::string_view text;
    std::string_view file;
    std::string_view component;
    // The author's stylesheet (issue #390), the page's second textarea. Empty means "no author
    // CSS", which keeps the built-in `preview_stylesheet()` fallback; a non-empty sheet REPLACES it
    // and never mixes with it (decision 2). It is part of the tuple `start()` compares, so a Run
    // that changed only the CSS re-evaluates instead of reusing the previous sheet.
    std::string_view stylesheet;
    std::string_view stylesheet_file = "ui/Preview.css";
    // The project salt the ids are hashed with; it must be the one the snapshot view uses, or the
    // canvas and the diagnostics pane would name one node by two different numbers.
    std::string_view salt = "preview";
    float width = 320.0f;
    float height = 240.0f;
    float dpi_scale = 1.0f;
    tessera::hal::FoldPosture posture = tessera::hal::FoldPosture::None;
    // The face the layout measures text with, or null for the deterministic stub. The session does
    // **not** own it: the device-backed stack belongs to whoever painted the last frame (the Wasm
    // entry, or a test), so the session stores the measurer `start()` was given and reuses it when
    // a Run re-evaluates. A font arriving later therefore needs a new `start()` - which is what the
    // page's `tessera_canvas_set_font` does when it arrives before the first Run.
    std::shared_ptr<const text::TextMeasurer> measurer;
};

// One evaluation, kept between events: the lowered IR the tree borrows from, the compiled sheet it
// was laid out against, and the `PreviewSession` that owns the tree and the router.
class CanvasSceneSession {
public:
    CanvasSceneSession();
    ~CanvasSceneSession();

    CanvasSceneSession(const CanvasSceneSession&) = delete;
    CanvasSceneSession& operator=(const CanvasSceneSession&) = delete;

    // Evaluates `source`, carrying the previous evaluation's state across the edit with the P4 rule
    // (component id + cell id + IR type; `scene_state.hpp`). Returns why a source produced no
    // scene, or an empty string on success.
    //
    // Calling it again is an *apply*: a new evaluation at the new source, with the old state
    // carried. A call with the identical source reuses the running evaluation instead, because the
    // page hands the editor's text to every frame and re-evaluating would drop hover and state
    // between two frames of one program. "Identical" is the SOURCE - text, stylesheet, file,
    // component and the measured face - never the viewport or the posture, which are re-applied to
    // the running tree in place (issue #436).
    //
    // **A source that does not evaluate leaves the running evaluation untouched** (issue #444). The
    // new text is parsed before anything is replaced, so a failed edit returns its reason while
    // `started()`, `root()`, the state cells and `frames_painted()` describe the tree that is still
    // on screen, and `needs_frame()` keeps whatever it already answered. That is what lets the
    // browser apply every keystroke without the canvas ever blanking: a half-typed expression costs
    // a diagnostic, not the picture. A cold start (a `restart`) is `tessera_canvas_reset` followed
    // by a `start()` - a different operation, and the page's Run button is the only caller.
    std::string start(const CanvasSessionSource& source);

    [[nodiscard]] bool started() const noexcept {
        return session_ != nullptr && session_->built();
    }
    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }
    // True when the last `start()` reused the running evaluation rather than re-evaluating.
    [[nodiscard]] bool reused() const noexcept {
        return reused_;
    }

    // Routes one normalized pointer event in logical viewport coordinates. `button` is the pointer
    // event's `button` (0 = primary); this slice only acts on the primary button, because a
    // secondary press dispatched as a left press would run an `onClick` nobody asked for.
    CanvasPointerResult pointer(PreviewInput::Kind kind, float x, float y,
                                std::uint32_t button = 0);

    // Changes the simulated fold posture of the RUNNING evaluation (issue #408): the tree is
    // re-laid out in place, so `SafeArea` padding and every box follow the new hinge without the
    // source being evaluated again and without hover, focus or the state cells being lost.
    //
    // **One layout pass, no stale frame.** The pass completes inside this call, before any paint
    // can observe the tree, so a page that repaints only when `needs_frame()` says so draws the new
    // posture on its first frame. Returns false when there is no running evaluation or the posture
    // is already the current one (nothing was laid out again).
    bool set_posture(tessera::hal::FoldPosture posture);

    // Resizes the RUNNING evaluation in place (issue #436): the device preset or an orientation
    // swap re-lays the tree out and re-resolves `@media` for the new rectangle - it never evaluates
    // the source again. That is the whole point: `start()` keeps its identity rule for SOURCE
    // changes only (width/height left `reused_ok`), so a viewport change keeps the state cells, the
    // hover and the focus the router holds, exactly like `set_posture()`.
    //
    // **One layout pass inside the call, and the cascade only when a breakpoint is crossed.** The
    // session asks the prepared sheet whether a `@media` answer can move
    // (`PreparedStyleSheet::media_answer_can_change`); if none can, the sheet and its paint cache
    // are untouched and this is a layout pass. If one can, the SAME prepared data is resolved
    // against the new context (ADR-048's resolve step) before the pass, and
    // `cascade_generation()` counts it. Safe-area padding is re-applied in the same pass by the
    // metrics the pass is given, so a resize to a foldable or phone size cannot show a frame with
    // stale padding.
    //
    // Returns false when nothing moved (no pass ran) or there is no running evaluation.
    bool set_viewport(float width, float height, float dpi_scale);

    // How many times the running evaluation's cascade has been resolved (issue #436): 1 at each
    // `start()`, +1 per `set_viewport` that crossed a breakpoint. The counter a test - and the
    // module's viewport report - use to tell "the cascade was rebuilt" from "the boxes moved".
    [[nodiscard]] std::uint64_t cascade_generation() const noexcept {
        return session_ != nullptr ? session_->cascade_generation() : 0;
    }

    // The `WindowMetrics` the live tree was laid out against, fold included; a fresh, fold-free
    // snapshot before `start()`.
    [[nodiscard]] const tessera::hal::WindowMetrics& metrics() const noexcept {
        return metrics_;
    }

    // True when the scene owes a frame: a fresh evaluation, or a dispatch that moved a style/paint
    // dirty domain since the last `paint()`. False for a static scene, which is the
    // render-on-demand contract the page's single-frame rule rests on.
    [[nodiscard]] bool needs_frame() const;

    // Paints the current tree into `target` and consumes the dirty domains, so a second `paint()`
    // with no intervening event paints the same pixels and `needs_frame()` stays false. The caller
    // owns `device` and `target`.
    //
    // `text` is the font the session paints with, and it is also the measurer the *layout* used
    // (issue #387): a session started with font bytes lays its text boxes out against that face, so
    // handing the same stack here keeps a painted line inside the box it was measured for. Null
    // paints boxes only, which is the state before any font arrived.
    SceneFrameResult paint(rhi::Device& device, rhi::RenderTarget& target,
                           const rhi::RenderTargetDesc& desc,
                           const rhi::PipelineDesc& pipeline = {},
                           PreviewTextStack* text = nullptr);

    [[nodiscard]] std::uint32_t frames_painted() const noexcept {
        return frames_painted_;
    }
    // The split of the cells the last *evaluating* apply carried, so the page can say which state
    // survived an edit and why the rest did not (`scene_state.hpp` is the rule). A non-zero
    // `dropped_component` is the structural change the spec calls a reset: a component the state
    // lived on is gone. A failed apply changes nothing here, like every other part of the running
    // evaluation it leaves alone.
    [[nodiscard]] const StateCarryReport& carry_report() const noexcept {
        return carry_report_;
    }
    // The live tree and the sheet it was laid out with; null before a successful `start()`. Exposed
    // so a caller can resolve computed style (the browser lane's colour comparison) and so a test
    // can assert what a pseudo-class resolved to without re-implementing the cascade.
    [[nodiscard]] const ui::Node* root() const noexcept;
    // The sheet the running tree is laid out with, which is NOT the one `start()` compiled once a
    // `set_viewport()` has crossed a breakpoint (issue #436): the session owns the current one, so
    // a painter and a test read it from here rather than keeping a copy that can go stale.
    [[nodiscard]] const ui::style::CompiledStyleSheet& sheet() const noexcept {
        return session_ != nullptr ? session_->sheet() : sheet_;
    }
    // The author stylesheet's parse problems from the last `start()` (issue #390), with line and
    // column. Survives the reuse path: a Run that changed nothing still reports the diagnostics the
    // sheet it kept produced, because the page's diagnostics area must not go blank on a re-Run.
    [[nodiscard]] const std::vector<PreviewCssDiagnostic>& css_diagnostics() const noexcept {
        return css_diagnostics_;
    }
    [[nodiscard]] ui::StateSnapshot state() const {
        return session_ != nullptr ? session_->state() : ui::StateSnapshot{};
    }
    // The router's own view of the interaction state. Its `hovered()`/`active()` are raw pointers
    // into the live tree, so they are only valid between a `start()` and the next one; a caller
    // that wants an id should read `CanvasPointerResult` instead.
    [[nodiscard]] const ui::InputRouter* router() const noexcept;

private:
    [[nodiscard]] bool reused_ok(const std::string& text, const std::string& stylesheet,
                                 const std::string& file, const std::string& component,
                                 const std::shared_ptr<const text::TextMeasurer>& measurer) const;

    // One layout pass for both layout inputs (viewport and posture), inside the call. Shared by
    // `set_posture()`, `set_viewport()` and the reuse path of `start()`, so a call that changes
    // both costs one pass, not two.
    [[nodiscard]] bool apply_layout_inputs(float width, float height, float dpi_scale,
                                           tessera::hal::FoldPosture posture);

    // The lowered IR the scene borrows its strings from: declared before the session so it outlives
    // the tree, which is the ownership rule `PreparedScene` states.
    EvaluatedScene evaluated_;
    ui::style::CompiledStyleSheet sheet_;
    std::unique_ptr<PreviewSession> session_;
    // Reused across frames so consuming the dirty domains allocates nothing on the frame path.
    ui::DirtyPass dirty_;
    // What the running evaluation was started from; a later `start()` with the same tuple reuses
    // it. Owned strings, because `CanvasSessionSource`'s views belong to the caller.
    std::string text_;
    std::string stylesheet_;
    std::string file_;
    std::string component_;
    // The author sheet's diagnostics from the last `start()` (issue #390), with line and column.
    std::vector<PreviewCssDiagnostic> css_diagnostics_;
    float width_ = 320.0f;
    float height_ = 240.0f;
    float dpi_scale_ = 1.0f;
    // The simulated fold posture the running tree was laid out at, so `set_viewport()` can re-lay
    // it out without changing it (issue #436).
    tessera::hal::FoldPosture posture_ = tessera::hal::FoldPosture::None;
    // The simulated window of the live tree. `session_` owns the authoritative copy; this one is
    // readable before the first `start()` and after the session is gone, so `metrics()` has one
    // answer in every state.
    tessera::hal::WindowMetrics metrics_;
    // The measurer the running evaluation was laid out with; a later `start()` compares it by
    // identity so a font arriving after the first Run is a re-evaluation, not a reuse.
    std::shared_ptr<const text::TextMeasurer> measurer_;
    std::string error_;
    bool reused_ = false;
    bool fresh_ = false; // a Run happened and its frame has not been painted yet
    std::uint32_t frames_painted_ = 0;
    // What the last evaluating apply carried; unchanged by an apply that failed to evaluate.
    StateCarryReport carry_report_;
};

} // namespace tessera::compiler::codegen::cli
