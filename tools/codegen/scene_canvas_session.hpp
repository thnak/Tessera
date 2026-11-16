// SPDX-License-Identifier: MIT

#pragma once

#include "scene_evaluate.hpp"
#include "scene_frame.hpp"
#include "scene_prepare.hpp"
#include "scene_session.hpp"
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
struct CanvasSessionSource {
    std::string_view text;
    std::string_view file;
    std::string_view component;
    // The project salt the ids are hashed with; it must be the one the snapshot view uses, or the
    // canvas and the diagnostics pane would name one node by two different numbers.
    std::string_view salt = "preview";
    float width = 320.0f;
    float height = 240.0f;
    float dpi_scale = 1.0f;
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
    // Calling it again is a *Run*: a new evaluation at the new source, with the old state carried.
    // A Run of the identical source reuses the running evaluation instead, because the page hands
    // the editor's text to every frame and re-evaluating would drop hover and state between two
    // frames of one program.
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
    // The live tree and the sheet it was laid out with; null before a successful `start()`. Exposed
    // so a caller can resolve computed style (the browser lane's colour comparison) and so a test
    // can assert what a pseudo-class resolved to without re-implementing the cascade.
    [[nodiscard]] const ui::Node* root() const noexcept;
    [[nodiscard]] const ui::style::CompiledStyleSheet& sheet() const noexcept {
        return sheet_;
    }
    [[nodiscard]] ui::StateSnapshot state() const {
        return session_ != nullptr ? session_->state() : ui::StateSnapshot{};
    }
    // The router's own view of the interaction state. Its `hovered()`/`active()` are raw pointers
    // into the live tree, so they are only valid between a `start()` and the next one; a caller
    // that wants an id should read `CanvasPointerResult` instead.
    [[nodiscard]] const ui::InputRouter* router() const noexcept;

private:
    [[nodiscard]] bool reused_ok(const std::string& text, const std::string& file,
                                 const std::string& component, float width, float height, float dpi,
                                 const std::shared_ptr<const text::TextMeasurer>& measurer) const;

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
    std::string file_;
    std::string component_;
    float width_ = 320.0f;
    float height_ = 240.0f;
    float dpi_scale_ = 1.0f;
    // The measurer the running evaluation was laid out with; a later `start()` compares it by
    // identity so a font arriving after the first Run is a re-evaluation, not a reuse.
    std::shared_ptr<const text::TextMeasurer> measurer_;
    std::string error_;
    bool reused_ = false;
    bool fresh_ = false; // a Run happened and its frame has not been painted yet
    std::uint32_t frames_painted_ = 0;
};

} // namespace tessera::compiler::codegen::cli
