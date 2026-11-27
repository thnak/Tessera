// SPDX-License-Identifier: MIT

#pragma once

#include "scene_canvas_inspect.hpp"
#include "scene_canvas_patch.hpp"
#include "scene_evaluate.hpp"
#include "scene_frame.hpp"
#include "scene_platform.hpp"
#include "scene_prepare.hpp"
#include "scene_project.hpp"
#include "scene_session.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/compiler/ir/source_span.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/responsive/environment_set.hpp"
#include "tessera/ui/responsive/size_class.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The canvas module's persistent session (issue #388, P3c): one evaluation that pointer events feed
// and that a frame paints, kept alive between events. It cannot be `prepare_scene()` per call: the
// tree, its `ui::InputRouter` (hover/active/focus/capture) and its state scope must outlive one
// pointer event and be visible to the next frame.
//
// **The pointer path** is `PreviewSession::dispatch`, the production `ui::InputRouter`
// (`src/ui/scene/input_dispatch.cpp`): hit testing, implicit capture, click synthesis and
// capture/target/bubble propagation. The router sets `PseudoState::Hover`/`Active`, and
// `style::SceneStyleTree` reads them as it computes, so `:hover`/`:active` match through the
// ordinary cascade; nothing here parses a selector.
//
// **`changed`, not "always redraw"**: a static UI presents nothing
// (docs/design/dirty-rendering.md), so "is a frame owed" is read from the scene's dirty domains.
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

    // Keyboard focus (issue #388), meaningful for `key()` and `blur()`; a pointer press also moves
    // focus, so `focused_id` is filled for every event.
    std::uint64_t focused_id = 0; // the focused node after the event, 0 when none
    bool focus_visible = false;   // the ring is showing (keyboard modality)
    bool focus_moved = false;     // this event changed which node holds focus
    // The key was consumed by the scene: the page prevents the browser's default (it moved focus,
    // activated a control, or reached a handler). A Tab at the end of the tab order is NOT handled:
    // the scene lets go and the browser moves on, so the keyboard never gets trapped in the canvas.
    bool handled = false;
    bool left = false; // Tab walked off the end of the tab order and focus was released
};

// What the picker found under one point (issue #171's picker slice): the node the pointer is over,
// named and boxed, or nothing.
//
// **Why this is a query and not a pointer event.** The picker must not change the scene: arming it
// and moving the pointer over a button must not set `:hover`, must not run a handler and must not
// cost a frame. So `pick()` reads the tree with the same hit test the router uses and returns a
// value; it dispatches nothing, marks nothing dirty and allocates nothing beyond the two borrowed
// strings it copies out.
//
// **Why the box is in the report.** A page that only knew the id could highlight nothing: the
// outline is drawn in the page's own coordinate space, so the engine has to say where the node is.
// `box` is `Node::world_bounds()` - the node's box in **logical viewport pixels**, the same space
// `pointer()` takes its coordinates in - so the page scales it by the CSS-to-logical ratio it
// already applies to pointer input and nothing else.
struct CanvasPickResult {
    bool found = false; // a node is under the point
    std::uint64_t automation_id = 0;
    // The node's element kind (`Button`, `Text`, `View`, ...), borrowed from the IR the running
    // evaluation owns, so it is valid only until the next `start()`. Empty when the node has none.
    std::string_view element_kind;
    // The node's box in logical viewport pixels. Zero when `found` is false.
    rhi::Rect box;
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
    // The single-file form, for a caller that has one source and (optionally) one stylesheet: a
    // project of one (`start(const CanvasSessionSource&)`). The stylesheet is a project file and
    // applies only when the source imports it (issue #390's "replace, never mix" rule, with the
    // sheets the program imports). A whole project goes through `start(project, source)`, which
    // does not read these four fields.
    std::string_view text;
    std::string_view file;
    std::string_view stylesheet;
    std::string_view stylesheet_file = "ui/Preview.css";
    std::string_view component;
    // The project salt the ids are hashed with; it must be the one the snapshot view uses, or the
    // canvas and the diagnostics pane would name one node by two different numbers.
    std::string_view salt = "preview";
    float width = 320.0f;
    float height = 240.0f;
    float dpi_scale = 1.0f;
    tessera::hal::FoldPosture posture = tessera::hal::FoldPosture::None;
    // The simulated skin's safe area (issue #171's skins slice): the insets a device would report
    // as `WindowMetrics::insets` and the cutout rectangles it would report as
    // `WindowMetrics::cutouts`. They are LAYOUT INPUTS like the size and the posture, not part of
    // the source's identity, so a skin change re-lays the running tree out in place
    // (`set_viewport`) and keeps hover, focus and the state cells. Zero/empty is the desktop answer
    // and the pre-#171 behaviour.
    tessera::hal::WindowInsets insets;
    tessera::hal::CutoutInfo cutouts;
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
    // **A source that does not evaluate leaves the running evaluation untouched** (issue #444): the
    // new text is parsed before anything is replaced, so a failed edit returns its reason while the
    // tree on screen, its state cells and `needs_frame()` are unchanged. A cold start (a `restart`)
    // is `tessera_canvas_reset` followed by a `start()`.
    std::string start(const CanvasSessionSource& source);
    // The same apply for a whole project: the entry's component, the files it imports and the
    // stylesheets those import. "Identical" then means every project file's path and bytes, the
    // entry, the component and the measured face.
    std::string start(const ProjectInput& project, const CanvasSessionSource& source);

    [[nodiscard]] bool started() const noexcept {
        return session_ != nullptr && session_->built();
    }
    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }
    // The components the running evaluation's ENTRY file declares, in source order, and the name of
    // the one it previews (issue #391's entry dropdown). They describe the tree still on screen, so
    // a half-typed edit that does not evaluate leaves the last good list in place.
    [[nodiscard]] const std::vector<std::string>& entry_components() const noexcept {
        return entry_components_;
    }
    [[nodiscard]] const std::string& component_name() const noexcept {
        return component_name_;
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

    // What is under `(x, y)` in logical viewport pixels, for the picker overlay (issue #171).
    //
    // **It reuses the router's hit test rather than writing a second one.** `Node::hit_test_deep`
    // is the function `ui::InputRouter` calls to decide a pointer target, so the node this reports
    // is exactly the node a press at the same point would reach - including `pointer-events: none`,
    // scrollport clipping, z-order and `visibility: hidden`.
    //
    // **It is a query, not an event.** Nothing is dispatched, no pseudo-class moves, no dirty
    // domain is set and no frame is owed: arming the picker and moving over the canvas must not
    // change the picture. That is why this is not a `pointer()` kind.
    //
    // Returns `found:false` for a point outside every node (and for a session with no tree), which
    // is the page's signal to hide the overlay rather than draw a stale outline.
    [[nodiscard]] CanvasPickResult pick(float x, float y);

    // The style inspector's query (issue #171, designer-protocol-v1 section 9.1): the computed
    // style, the matched rules with their provenance, and the box of the node with this automation
    // id.
    //
    // **It is a query, like `pick()`.** Nothing is dispatched, no pseudo-class moves and no frame
    // is owed: opening the inspector on a node must not change the picture. It is a user action
    // rather than a frame path, so the bounded allocations it makes are acceptable - the frame
    // path's zero-allocation rule is about `paint()`, not about a click.
    //
    // **Why the engine answers it.** `computed`, `matched` and `overridden` are the cascade's own
    // answers; a page that recomputed them would be a second cascade that could disagree with the
    // canvas. `automation_id` 0 and an id no node carries both answer `found:false`, which the
    // report spells `no_such_node` (designer-protocol-v1 section 9.1).
    [[nodiscard]] StyleInfoResult inspect(std::uint64_t automation_id);

    // The designer's override layer (issue #536, designer-protocol-v1 sections 9.2-9.4): values a
    // developer tries on one node before committing them. Every operation restyles the RUNNING
    // tree - a layout pass inside the call, never an evaluation - so the state cells, hover and
    // focus survive, and a frame is owed afterwards (`needs_frame()`).
    //
    // **Validation is the stylesheet engine's** (`validate_override`): a refused declaration is
    // `ok:false` with the `TESSERA_CSS_*` diagnostic and leaves the node's overrides, the history
    // and the tree exactly as they were.
    //
    // **An override changes only the node it names**, and belongs to the session: it is lost when
    // the session ends, is never written to source without a commit, and survives a re-evaluation
    // (re-applied to each tree built; one whose node an edit removed stays unapplied until
    // cleared).
    [[nodiscard]] OverrideOutcome override_set(std::uint64_t automation_id,
                                               std::string_view property, std::string_view value);
    // Removes one override of a node, or every override of it when `property` is empty.
    [[nodiscard]] OverrideOutcome override_clear(std::uint64_t automation_id,
                                                 std::string_view property = {});
    // Removes every override in the viewer.
    [[nodiscard]] OverrideOutcome override_revert();
    // `edit.undo` / `edit.redo` (section 9.4): step the history of `set`/`clear`/`revert`. A commit
    // is not part of it (`settle_commit` consumes its operations).
    [[nodiscard]] OverrideOutcome edit_undo();
    [[nodiscard]] OverrideOutcome edit_redo();
    // The node's current overrides, in the order they were set.
    [[nodiscard]] std::vector<StyleOverrideInfo> overrides_of(std::uint64_t automation_id) const;

    // The host round trip's last step (section 10.3), called after `source.patch.result` said
    // `applied` AND the following `source.update` has been evaluated (`start()`): the node's
    // overrides leave the layer if the new evaluation gives the node the computed values they
    // produced; otherwise they stay and the answer carries a `TESSERA_DESIGNER_PATCH_MISMATCH`
    // diagnostic, so a commit that did not round-trip is visible rather than silently dropped.
    //
    // **The comparison is the engine's, not a string match**: the values with the overrides on
    // are read, the overrides are taken off, and the values of every longhand the overrides touch
    // are read again. Equal means the source now produces what the override did.
    [[nodiscard]] CommitSettleResult settle_commit(std::uint64_t automation_id);

    // Changes the simulated fold posture of the RUNNING evaluation (issue #408): the tree is
    // re-laid out in place, so `SafeArea` padding and every box follow the new hinge without the
    // source being evaluated again and without hover, focus or the state cells being lost.
    //
    // **One layout pass, no stale frame.** The pass completes inside this call, before any paint
    // can observe the tree, so a page that repaints only when `needs_frame()` says so draws the new
    // posture on its first frame. Returns false when there is no running evaluation or the posture
    // is already the current one.
    bool set_posture(tessera::hal::FoldPosture posture);

    // Resizes the RUNNING evaluation in place (issue #436): the device preset or an orientation
    // swap re-lays the tree out and re-resolves `@media` - it never evaluates the source again.
    // `start()` keeps its identity rule for SOURCE changes only (width/height left `reused_ok`), so
    // a viewport change keeps the state cells, the hover and the focus, like `set_posture()`.
    //
    // **One layout pass inside the call, and the cascade only when a breakpoint is crossed.** The
    // sheet is asked whether a `@media` answer can move
    // (`PreparedStyleSheet::media_answer_can_change`); if none can, it and its paint cache are
    // untouched and this is a layout pass. If one can, the SAME prepared data is resolved against
    // the new context (ADR-048's resolve step) before the pass, and `cascade_generation()` counts
    // it. False when nothing moved (no pass ran) or there is no running evaluation.
    bool set_viewport(float width, float height, float dpi_scale);

    // The scale the tree was laid out at; `paint()` scales the frame by it (issue #599).
    [[nodiscard]] float dpi_scale() const noexcept;

    // The same call for a skin (issue #171's skins slice): the size, the DPI scale, the safe-area
    // insets and the cutout rectangles all move together, in ONE layout pass. It is a separate
    // spelling rather than more arguments on `set_viewport` because the two callers are different
    // controls - the toolbar's size/DPR selects and the skin picker - and a skin change that kept
    // the previous insets would pad the content for a device that is no longer on screen.
    //
    // Returns false when nothing moved (no pass ran) or there is no running evaluation.
    bool set_safe_area(const tessera::hal::WindowInsets& insets,
                       const tessera::hal::CutoutInfo& cutouts);

    // One keyboard event (issue #388, focus). `key` is a `ui::KeyCode`; `modifiers` a
    // `ui::Modifier` mask. Tab and Shift+Tab move focus through the focusable controls in tree
    // order and **release it at either end** instead of wrapping, so a page can let the browser
    // carry on to the next element; Enter and Space activate the focused button (the router's click
    // synthesis); the `:focus-visible` ring shows after keyboard traversal.
    CanvasPointerResult key(PreviewInput::Kind kind, std::uint32_t key, std::uint32_t modifiers,
                            bool repeat);

    // The page's element lost the keyboard: drop focus (and the ring) so a canvas that is not
    // focused does not look as if it were. A frame is owed only when something was focused.
    CanvasPointerResult blur();
    // `platform.event` (designer-protocol-v1 section 5.1, issue #446): one normalized hardware
    // button or environment event, through `PlatformEventDriver` (`scene_platform.hpp`). A relaunch
    // reply ends this evaluation so the page's next `start()` is a cold start, carrying the cells
    // saved at the last Suspend when `restore` is set and nothing otherwise.
    [[nodiscard]] PlatformReply platform_event(const PlatformEvent& event);
    [[nodiscard]] AppLife app_life() const noexcept {
        return platform_.life();
    }

    // How many times the running evaluation's cascade has been resolved (issue #436): 1 at each
    // `start()`, +1 per `set_viewport` that crossed a breakpoint. The counter a test - and the
    // module's viewport report - use to tell "the cascade was rebuilt" from "the boxes moved".
    [[nodiscard]] std::uint64_t cascade_generation() const noexcept {
        return session_ != nullptr ? session_->cascade_generation() : 0;
    }
    // The ADR-041 settle counters of the running session (issue #693): deliveries, rounds, engine
    // rounds, deferred observations and round-limit hits. The same numbers `AppHost` reports.
    [[nodiscard]] const tessera::ui::layout::SettleTotals& settle_totals() const noexcept {
        static const tessera::ui::layout::SettleTotals kNone{};
        return session_ != nullptr ? session_->settle_totals() : kNone;
    }

    // The `useBreakpoint()` class of the running viewport (issue #692), and the consumer set the
    // page's components register with. The canvas pushes the class from `set_viewport`, exactly as
    // `AppHost` does from `apply_viewport`, so the preview and the runtime cannot disagree.
    [[nodiscard]] tessera::ui::responsive::SizeClass size_class() const noexcept {
        return session_ != nullptr ? session_->size_class()
                                   : tessera::ui::responsive::SizeClass::Compact;
    }
    [[nodiscard]] tessera::ui::responsive::EnvironmentSet& environment_values() noexcept {
        return environment_;
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
    // handing the same stack here keeps a painted line inside the box it was measured for.
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
    // lived on is gone. A failed apply changes nothing here.
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

    // The IR source span of the node with this automation id (issue #171's `source.patch` slice),
    // or null when the id names no node or the node has no span. The span is the JSX element's, so
    // a commit can locate the attribute it edits; it belongs to the running evaluation and is valid
    // until the next `start()`.
    [[nodiscard]] const ir::SourceSpan* node_span(std::uint64_t automation_id) const;

    // The project the running evaluation was started from, by reference: the bytes a patch's
    // offsets are measured in. Empty before a successful `start()`. It is the session's own copy,
    // so it outlives every call and a patch built from it cannot outlive its text.
    [[nodiscard]] const OwnedProject& project() const noexcept {
        return project_;
    }

    // Stages the theme asset (`assets.theme`, stylesheet-v1 section 9.1) a `token` commit edits:
    // its project-relative path, its bytes and the id of the active theme. The preview never
    // compiles the asset, so the session holds the bytes only to measure a token edit's offsets in
    // (issue #536). Without it a `token` commit is `not_token_bound`. It belongs to the session,
    // not to an evaluation, so a re-evaluation keeps it; staging again replaces it.
    void set_theme_asset(std::string file, std::string text, std::string theme_id);

    // The patch for one style commit (issue #171, designer-protocol-v1 section 10.2): the minimal
    // edit list and the base versions, or the spec's reason there is none.
    //
    // **It is a query.** Nothing is written, nothing is dispatched and no frame is owed: a commit
    // produces a value the host applies, and the viewer keeps its overrides until
    // `source.patch.result` arrives (section 10.3).
    [[nodiscard]] SourcePatchResult patch(const PatchRequest& request) const;

private:
    // Fills `focused_id` / `focus_visible` from the router's focus manager.
    void fill_focus(CanvasPointerResult& result) const;

    [[nodiscard]] bool reused_ok(const ProjectInput& project, const std::string& component,
                                 const std::shared_ptr<const text::TextMeasurer>& measurer) const;

    // One layout pass for both layout inputs (viewport and posture), inside the call. Shared by
    // `set_posture()`, `set_viewport()` and the reuse path of `start()`, so a call that changes
    // both costs one pass, not two.
    [[nodiscard]] bool apply_layout_inputs(float width, float height, float dpi_scale,
                                           tessera::hal::FoldPosture posture);

    // The same pass with the skin's safe area as well: every layout input a skin can move, in one
    // call. `apply_layout_inputs` is the narrow spelling that keeps the current insets.
    [[nodiscard]] bool apply_layout_inputs(float width, float height, float dpi_scale,
                                           tessera::hal::FoldPosture posture,
                                           const tessera::hal::WindowInsets& insets,
                                           const tessera::hal::CutoutInfo& cutouts);

    // The lowered IR the scene borrows its strings from: declared before the session so it outlives
    // the tree, which is the ownership rule `PreparedScene` states.
    // The override layer outlives every `PreviewSession`, which only borrows it: each tree the
    // session builds has the layer applied, so an override survives a rebuild and a re-evaluation.
    OverrideLayer overrides_;
    // The staged theme asset for `token` commits (`set_theme_asset`).
    std::string theme_file_;
    std::string theme_text_;
    std::string theme_id_;
    EvaluatedScene evaluated_;
    ui::style::CompiledStyleSheet sheet_;
    std::unique_ptr<PreviewSession> session_;
    // The `useBreakpoint()` consumers of this page (issue #692). It belongs to the session, not to
    // an evaluation, so a re-evaluation keeps the registrations a component re-attaches anyway.
    tessera::ui::responsive::EnvironmentSet environment_;
    // Reused across frames so consuming the dirty domains allocates nothing on the frame path.
    ui::DirtyPass dirty_;
    // What the running evaluation was started from; a later `start()` with the same tuple reuses
    // it. Owned strings, because `CanvasSessionSource`'s views belong to the caller.
    OwnedProject project_;
    std::string component_;
    std::vector<std::string> entry_components_;
    std::string component_name_;
    // The author sheet's diagnostics from the last `start()` (issue #390), with line and column.
    std::vector<PreviewCssDiagnostic> css_diagnostics_;
    float width_ = 320.0f;
    float height_ = 240.0f;
    float dpi_scale_ = 1.0f; // the layout viewport's device-pixel scale; `paint()` reads it (#599)
    // The simulated fold posture the running tree was laid out at, so `set_viewport()` can re-lay
    // it out without changing it (issue #436).
    tessera::hal::FoldPosture posture_ = tessera::hal::FoldPosture::None;
    // The skin's safe area the running tree was laid out at (issue #171), so a size or posture
    // change keeps it and a skin change replaces it. Zero/empty before the first `start()`.
    tessera::hal::WindowInsets insets_;
    tessera::hal::CutoutInfo cutouts_;
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
    // The app's life state, and the cells a relaunch carries into the next evaluation.
    PlatformEventDriver platform_;
    std::optional<CarriedState> relaunch_carry_;
};
} // namespace tessera::compiler::codegen::cli