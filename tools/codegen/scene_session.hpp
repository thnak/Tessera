// SPDX-License-Identifier: MIT

#pragma once

#include "scene_build.hpp"
#include "scene_layout.hpp"
#include "scene_override.hpp"
#include "scene_state.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tessera/ui/behavior/instance.hpp"
#include "tessera/ui/layout/layout_settle.hpp"
#include "tessera/ui/responsive/environment_set.hpp"
#include "tessera/ui/responsive/size_class.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/state_snapshot.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// The preview's interaction session (issue #168, P4 of docs/design/reference-equantic-ui.md section
// 7.4): one in-memory evaluation that browser input can be routed into, whose state cells survive a
// source edit by component identity and cell id (`scene_state.hpp` states that rule).
//
// **What a synthetic click actually invokes.** The preview is an IR *evaluator*: it does not run
// the developer's C++. A click goes through the engine's own `ui::InputRouter`
// (`src/ui/scene/input_dispatch.cpp`), so hit testing, implicit pointer capture, focus, click
// synthesis on the nearest common ancestor and W3C capture/target/bubble propagation are the
// production code paths, identical to native. What that path reaches is a `Node::bind_event`
// callback this session installs; the callback runs the IR *preview body* of the node's handler
// through `eval::Evaluator::run` — the same action list `tests/parity/parity_runner.cpp` executes
// for a `click <automationId>` step. Consequences, reported rather than implied:
//   * a native binding with no preview body does nothing here, exactly as in the parity runner;
//   * a handler that declares a payload parameter (`(e: ClickEvent) => ...`) cannot run, because
//     the evaluator has no record value to bind to it: it is counted in `unrun`;
//   * the developer's C++ is never executed, and this session makes no claim to execute it.
namespace tessera::compiler::codegen::cli {

// The text module this session's measurement seam names lives one namespace up.
namespace text = tessera::ui::text;

// One normalized input event, as the browser page produces it: the subset of `ui::InputEvent` a
// pointer or key gesture needs, flat because it crosses a C ABI (`wasm_scene_entry.cpp`).
struct PreviewInput {
    enum class Kind : std::uint8_t {
        PointerMove,
        PointerDown,
        PointerUp,
        PointerCancel,
        KeyDown,
        KeyUp,
    };
    Kind kind = Kind::PointerDown;
    float x = 0.0f;
    float y = 0.0f;
    // Bitmask of `ui::Modifier` (Shift 1, Control 2, Alt 4, Meta 8, CapsLock 16, NumLock 32), so
    // the page passes its `shiftKey`-style booleans through one integer.
    std::uint32_t modifiers = 0;
    std::uint32_t key = 0; // a `ui::KeyCode`; 0 when not a key event
    bool repeat = false;   // auto-repeat of a held key (Enter does not activate on a repeat)
    std::uint32_t pointer_id = 0;
};

// The engine's normalized event for `input`. `timestamp_s` comes from the session's fixed step
// counter (never a wall clock), so two runs of one script produce identical events.
[[nodiscard]] ui::InputEvent to_input_event(const PreviewInput& input, double timestamp_s);

// What a scripted click did. A miss (no node with that automation id) and a handler this session
// cannot run are separate results, because they are different failures for a page author.
struct PreviewClickResult {
    bool found = false;      // a visible node carries that automation id
    bool dispatched = false; // the press reached a scene target
    std::uint64_t target_id = 0;
    std::vector<std::string> ran;   // handler ids whose IR body executed, in dispatch order
    std::vector<std::string> unrun; // handler ids the dispatch reached but could not execute
};

// One evaluation. Construction seeds the scope (carried cells first, then initializers), builds the
// scene, binds the IR handlers, resolves style and lays the tree out.
//
// **Blocking and threading:** a session is a value the caller owns on one thread. Nothing here
// starts a thread, waits, reads a clock, touches the filesystem or performs I/O; a page calls it
// between frames. The scene borrows the IR's strings, so the module and component must outlive it.
class PreviewSession {
public:
    struct Options {
        float width = 320.0f;
        float height = 240.0f;
        float dpi_scale = 1.0f;
        // The simulated foldable posture (issue #408). One evaluation lays its tree out once at
        // this posture; `set_posture()` re-lays it out in place for a later change, so a posture
        // switch costs no re-evaluation and keeps the state the session has accumulated.
        tessera::hal::FoldPosture posture = tessera::hal::FoldPosture::None;
        // The simulated skin's safe area (issue #171's skins slice): the insets and cutout
        // rectangles a device would report. Layout inputs like the size and the posture, so a skin
        // change re-lays the tree out in place and keeps the state the session has accumulated.
        tessera::hal::WindowInsets insets;
        tessera::hal::CutoutInfo cutouts;
        // The media-independent sheet and the media context it is resolved against (issue #436).
        // A session that owns the prepared form can re-resolve `@media` IN PLACE when its viewport
        // changes (`set_viewport`), instead of being re-evaluated from source. Empty `prepared`
        // keeps the caller's already-compiled `sheet` for the session's whole life, which is what
        // every caller before this issue passes.
        std::optional<PreparedPreviewSheets> prepared;
        tessera::ui::style::MediaContext media;
        // State carried from the previous evaluation of an edited source. `live_component_ids` is
        // every component id the new evaluation has, so a cell whose component disappeared is
        // reported as dropped instead of being offered to a component that took its place.
        const CarriedState* carried = nullptr;
        const std::vector<std::string>* live_component_ids = nullptr;
        // The text measurement seam for every layout this session runs, including the one a
        // post-dispatch rebuild performs. Null keeps the deterministic stub the font-free preview
        // has always used; a real face measures the boxes its glyphs will actually fill (issue
        // #387).
        std::shared_ptr<const text::TextMeasurer> measurer;
        // The designer's override layer (designer-protocol-v1 section 9.2), or null for none. NOT
        // owned: the canvas session owns the layer and it outlives every tree, so each rebuild
        // re-applies the same entries to the tree it just built.
        const OverrideLayer* overrides = nullptr;
    };

    PreviewSession(const ir::IrModule& module, const ir::Component& component,
                   const ui::style::CompiledStyleSheet& sheet, Options options);
    ~PreviewSession();

    PreviewSession(const PreviewSession&) = delete;
    PreviewSession& operator=(const PreviewSession&) = delete;

    // Routes one event through `ui::InputRouter`. False when nothing received it. Handler bodies
    // run inline, in the dispatch's own capture/target/bubble order; the tree is rebuilt afterwards
    // if any of them changed state, never during the dispatch.
    bool dispatch(const PreviewInput& input);

    // The gesture a scripted `click <automationId>` step is: find the node by automation id under
    // the live tree, then press and release at its centre, so hit testing and click synthesis
    // decide the target exactly as they do for a real pointer — the same approach as
    // `EngineAutomationHost::click` in the native harness.
    [[nodiscard]] PreviewClickResult click(std::uint64_t automation_id);

    // Every state and animated cell in declaration order, in the existing `ui::StateCell`
    // representation: what generated `snapshot_state()` returns and the parity runner compares.
    [[nodiscard]] ui::StateSnapshot state() const;
    // This session's state, for handing to the next evaluation after an edit.
    [[nodiscard]] CarriedState capture() const;

    // Changes the simulated fold posture and re-lays THIS tree out in place (issue #408). Nothing
    // is re-evaluated: the scope, the hover/active state and the focus the router holds all
    // survive, which is what makes a posture switch in the studio feel like turning a device rather
    // than running a program.
    //
    // **One layout pass, before anything is painted.** The pass below writes every `SafeArea`
    // node's padding and every box from the new metrics before it returns, so a caller that only
    // paints after the call can never observe the previous posture's padding. The dirty domains the
    // pass sets are what tells a render-on-demand caller that a frame is owed.
    //
    // Returns false when the posture was already the requested one (and therefore nothing was laid
    // out again) or when this session built no tree; true when a layout pass ran.
    bool set_posture(tessera::hal::FoldPosture posture);

    // Resizes the RUNNING tree in place (issue #436): the viewport and the DPI scale are layout
    // inputs like the posture, so a device preset or an orientation swap re-lays the tree out and
    // re-resolves `@media` - it never evaluates the source again. Hover, focus, the state cells and
    // the router's capture all survive, which is what a breakpoint is supposed to feel like in a
    // preview.
    //
    // `@media` is re-resolved only when the move can change an answer
    // (`PreparedStyleSheet::media_answer_can_change`): a size that crosses no breakpoint is one
    // layout pass and no cascade rebuild. When it does cross one, the SAME prepared data is
    // resolved against the new context - the ADR-048 resolve step, never a re-parse - so the frame
    // the caller paints next is already the responsive one. `cascade_generation()` counts those
    // rebuilds.
    //
    // Returns false when nothing moved (no layout pass ran) or the session built no tree.
    bool set_viewport(float width, float height, float dpi_scale);

    // One pass for both layout inputs, so a call that changes the size AND the posture costs one
    // layout pass, not two. The two setters above are the narrow spellings of this.
    bool relayout_at(float width, float height, float dpi_scale, tessera::hal::FoldPosture posture);

    // The same pass with the skin's safe area (issue #171's skins slice): the size, the DPI scale,
    // the posture, the insets and the cutout rectangles all move together, in ONE layout pass. A
    // skin whose size matches the previous one but whose insets differ is still a different device,
    // so the insets are part of the "did anything move?" test rather than a separate call.
    bool relayout_at(float width, float height, float dpi_scale, tessera::hal::FoldPosture posture,
                     const tessera::hal::WindowInsets& insets,
                     const tessera::hal::CutoutInfo& cutouts);

    // Brings the live tree in line with the override layer after the layer changed (designer-
    // protocol-v1 sections 9.2-9.4): every override the tree carries is taken off, the layer's
    // current entries are put on, and the tree is laid out again, in ONE call - a restyle, never a
    // re-evaluation, so the state cells, hover and focus survive. A frame is owed afterwards.
    // False when the session built no tree.
    bool resync_overrides();

    // How many times this session's cascade has been resolved: 1 at construction, +1 per re-resolve
    // (`set_viewport` crossing a breakpoint). The counter a test asserts on to tell "the cascade
    // was rebuilt" from "the boxes were laid out again" (issue #436).
    [[nodiscard]] std::uint64_t cascade_generation() const noexcept {
        return cascade_generation_;
    }

    // The sheet the tree is CURRENTLY laid out with. After `set_viewport` crosses a breakpoint this
    // is a different sheet from the one the session was constructed with, which is why a painter
    // must read it here rather than keep its own copy.
    [[nodiscard]] const ui::style::CompiledStyleSheet& sheet() const noexcept {
        return sheet_;
    }

    // The ADR-041 settle loop of this session (issue #693): the same shared component `AppHost`
    // runs, so the preview cannot drift from the runtime. It is driven from `rebuild()` and
    // `relayout()`, which publish the layout and then run the loop, so a `@container` rule that
    // crossed a condition has re-styled before the caller paints. The canvas has no
    // `UpdateScheduler`, which the loop accepts as "no app callbacks to flush".
    [[nodiscard]] const ui::layout::LayoutSettle& settle() const noexcept {
        return settle_;
    }
    // The settle counters, for a report or a test: rounds, engine rounds, deferred observations and
    // round-limit hits.
    [[nodiscard]] const ui::layout::SettleTotals& settle_totals() const noexcept {
        return settle_.totals();
    }

    // The `useBreakpoint()` consumers of this session (issue #692). The canvas pushes the class
    // from `set_viewport`, exactly as `AppHost` does from `apply_viewport`, so the preview and the
    // runtime cannot disagree about which class a width is in.
    [[nodiscard]] ui::responsive::EnvironmentSet& environment_values() noexcept {
        return environment_;
    }
    // The size class in force, computed from the same media context the `@media` rules resolve
    // against.
    [[nodiscard]] ui::responsive::SizeClass size_class() const noexcept {
        return ui::responsive::size_class_for(media_);
    }

    // The `WindowMetrics` the current tree was laid out against, fold included. A caller reports or
    // compares what the tree was actually sized to rather than rebuilding the snapshot.
    [[nodiscard]] const tessera::hal::WindowMetrics& metrics() const noexcept {
        return metrics_;
    }

    [[nodiscard]] const eval::Scope& scope() const noexcept {
        return scope_;
    }
    [[nodiscard]] const ui::Node& root() const noexcept {
        return *root_;
    }
    [[nodiscard]] const ui::InputRouter& router() const noexcept {
        return router_;
    }
    // The mutable router, for the host operations that are not events: releasing focus when the
    // page's element loses it, and leaving the tab order at its ends (issue #388, focus).
    [[nodiscard]] ui::InputRouter& router() noexcept {
        return router_;
    }
    [[nodiscard]] ui::Node& root() noexcept {
        return *root_;
    }
    [[nodiscard]] const StateCarryReport& carry_report() const noexcept {
        return carry_report_;
    }
    [[nodiscard]] std::size_t bound_handlers() const noexcept {
        return slots_.size();
    }
    [[nodiscard]] bool built() const noexcept {
        return built_;
    }
    // Why the tree could not be built (a `TESSERA_SCENE_UNSUPPORTED` sentence naming the
    // construct), or empty when it was. The canvas reports it, so a person reads "a user component
    // instance needs prop binding..." instead of a generic refusal.
    [[nodiscard]] const std::string& build_error() const noexcept {
        return build_error_;
    }
    // What the most recent `dispatch()` ran and could not run, in binding order. A canvas caller
    // reports these to the page so a handler this slice cannot execute is visible rather than
    // silent, the same split `PreviewClickResult` makes.
    [[nodiscard]] const std::vector<std::string>& last_ran() const noexcept {
        return ran_;
    }
    [[nodiscard]] const std::vector<std::string>& last_unrun() const noexcept {
        return unrun_;
    }

    // The behavior instances this session drives (ADR-064 item 7; behavior-hooks-v1.md section 10).
    // One per IR `behaviors` entry, in declaration order, attached after the tree is built and
    // detached before it is replaced. Empty for a component that calls no behavior hook, which is
    // every component today unless the experimental front end produced one.
    //
    // **Why the session drives them at all.** The preview is an IR evaluator: it does not run the
    // developer's C++. A behavior is engine code, so the session can and must run it -- otherwise
    // Studio would show a component's static result and a person would see a Button that never
    // looks pressed. The session builds the same `behavior::Instance` the generated base does, from
    // the same IR entry and the same registry, so the two cannot disagree about what a behavior is.
    [[nodiscard]] std::size_t behavior_count() const noexcept {
        return behaviors_.size();
    }
    // The current outputs of behavior `index`, or an empty span when there is no such behavior or
    // it is not attached. A caller reads its manifest defaults in that case.
    [[nodiscard]] std::span<const ui::behavior::OutputValue>
    behavior_outputs(std::size_t index) const;
    // The engine registry name of behavior `index` (`press_feedback`), or empty.
    [[nodiscard]] std::string_view behavior_name(std::size_t index) const;
    // True when behavior `index` is attached to every target its refs resolved to.
    [[nodiscard]] bool behavior_attached(std::size_t index) const;

    // The IR source span of every node in the live tree, keyed by scene node id (issue #171's
    // `source.patch` slice). Filled by the build walk, which is the last code that holds the
    // `ir::Node`; a rebuild replaces it, so a span always describes the tree on screen. Empty for a
    // session that built no tree.
    //
    // **Why a side table and not a `ui::Node` field.** `ui::Node` is a public header with an
    // enforced `kNodeSizeBudget` and a `static_assert`; a span is a design-time fact no frame
    // reads, so growing every node for it would be a memory-budget decision taken for a query. The
    // table is the session's, so it dies with the tree it describes.
    [[nodiscard]] const std::vector<SceneBuildResult::SceneNodeSource>&
    node_sources() const noexcept {
        return node_sources_;
    }

    // The node with this automation id, or null. A read-only lookup for a caller that needs a fact
    // about the node (its class name, for a `class` commit) without a mutable handle into the tree.
    [[nodiscard]] const ui::Node* find_by_automation_id(std::uint64_t automation_id) const;

private:
    // One installed scene binding: which IR handler a scene node's event must run. Kept in a
    // `unique_ptr` so the `void*` context handed to `Node::bind_event` keeps a stable address.
    struct HandlerSlot;
    // One installed `onSizeChange` observation (ADR-041 item 3, issue #694). The preview is an IR
    // evaluator, so it cannot run the developer's C++ thunk; it runs the same IR body the generated
    // base would, with a `SizeChangeEvent` record bound to the handler's parameter. The slot is
    // kept in a `unique_ptr` for the same reason `HandlerSlot` is: `LayoutObservation` holds the
    // context pointer for its whole life.
    struct SizeSlot;
    // Attaches one observation per `onSizeChange` node of the tree just built, after layout, so the
    // first delivery carries the size the tree actually has. Replaced by every `rebuild()`.
    void attach_size_observations(const SceneBuildResult& built);
    // Detaches every observation before the tree it targets is destroyed.
    void detach_size_observations();
    // The `LayoutObservation` callback: runs the slot's IR body with a `SizeChangeEvent` record.
    static void on_size_change(void* context, const ui::layout::SizeChange& size);
    // The settle loop's relayout hook: a round whose callback changed state re-evaluates the tree.
    static void settle_relayout(void* context);

    void rebuild();
    // The override layer's entries put onto the tree `rebuild()` just built; takes nothing off.
    void apply_overrides();
    // Takes every applied override off the live tree, restoring the inline value it replaced.
    void restore_overrides();
    // Lays `root_` out against `layout_metrics_` and the current `sheet_`. One pass, no
    // re-evaluation: everything a size or posture change needs, and nothing else.
    [[nodiscard]] bool relayout();
    void bind_handler(std::uint64_t automation_id, const std::string& event,
                      const std::string& handler_id);
    // Attaches every IR behavior to the nodes its target refs resolved to, after the tree exists.
    // A behavior whose targets are not all present attaches nothing (the spec's rule), and a
    // behavior the registry lacks is reported in `behavior_errors_` rather than silently skipped.
    void attach_behaviors(const SceneBuildResult& built);
    // Recomputes every attached behavior's outputs from the scene's current interaction state. This
    // is the scene-driven trigger: a pointer moved or a press began while the component's own
    // options did not change, so the option hash would skip the update forever.
    void refresh_behaviors();
    // Detaches every behavior before the tree it targets is destroyed.
    void detach_behaviors();
    static void on_scene_event(void* context, const ui::Event& event);
    void run_handler(const std::string& handler_id, const std::vector<ir::Action>* actions);
    [[nodiscard]] ui::Node* node_by_automation_id(std::uint64_t automation_id);
    // The node with this SCENE id (the identity a `ref` resolved to), or null. Distinct from
    // `node_by_automation_id`: a scene id is assigned by the build walk in preorder, an automation
    // id comes from the IR's `automationId`/`uiId`. A behavior targets the former.
    [[nodiscard]] ui::Node* node_by_id(std::uint64_t node_id);
    void append_cell(ui::StateSnapshot& out, const std::string& id, const std::string& type) const;

    const ir::IrModule& module_;
    const ir::Component& component_;
    ui::style::CompiledStyleSheet sheet_;
    Options options_;
    // The ADR-041 settle loop (issue #693): the read model, the container-query tracker and the
    // app observations, shared with `AppHost` so the preview and the runtime cannot disagree.
    ui::layout::LayoutSettle settle_;
    // The `useBreakpoint()` consumers (issue #692), and the class last pushed so a resize inside
    // one class walks nothing.
    ui::responsive::EnvironmentSet environment_;
    ui::responsive::SizeClass pushed_size_class_ = ui::responsive::SizeClass::Compact;
    // Pushes the current size class to every `useBreakpoint()` consumer (a no-op when it did not
    // move).
    void push_size_class();
    // The media context `sheet_` was resolved against (issue #436), so a viewport change can ask
    // whether a `@media` answer moves before resolving anything.
    ui::style::MediaContext media_;
    // What `sheet_`'s `env()` was resolved against (ADR-074), so a safe-area or posture change can
    // ask whether a variable a sheet reads moved before resolving anything.
    ui::style::StyleEnvironment style_environment_;
    std::uint64_t cascade_generation_ = 1;
    eval::Scope scope_;
    StateCarryReport carry_report_;
    std::unique_ptr<ui::Node> root_;
    ui::InputRouter router_;
    eval::Evaluator evaluator_;
    // The simulated window the live tree was laid out against, fold included (issue #408). Kept
    // because a later posture or size change must lay out against the same rectangle and the same
    // DPI scale, not against defaults.
    SceneMetrics layout_metrics_;
    hal::WindowMetrics metrics_;
    std::vector<std::unique_ptr<HandlerSlot>> slots_;
    // One entry per `onSizeChange` node of the live tree (issue #694), in build order. Replaced by
    // every `rebuild()`, and detached before the tree it observes dies.
    std::vector<std::unique_ptr<SizeSlot>> size_slots_;
    // One behavior instance per IR `behaviors` entry, in declaration order (ADR-064 item 7). The
    // evaluated args are kept alongside because `refresh` re-passes them: a behavior reads its
    // options on every recompute, and re-evaluating them per pointer move would be work for a value
    // that did not change.
    struct BehaviorSlot {
        std::string name; // the engine registry name
        ui::behavior::Instance instance;
        std::vector<ui::behavior::ArgValue> args;
        // The `ref::` ids this behavior targets, in manifest order, and the scene node each one
        // resolved to. A node id of 0 means the ref did not resolve (a branch that did not take).
        std::vector<std::uint64_t> target_nodes;
    };
    std::vector<BehaviorSlot> behaviors_;
    // Why a behavior could not attach, in declaration order. Reported rather than swallowed: a
    // manifest entry the registry lacks is a build-tool error a person should see.
    std::vector<std::string> behavior_errors_;
    std::vector<std::string> ran_;
    std::vector<std::string> unrun_;
    // The IR spans of the live tree, keyed by scene node id (issue #171's `source.patch` slice).
    // Replaced by every `rebuild()`, so it never describes a tree that is gone.
    std::vector<SceneBuildResult::SceneNodeSource> node_sources_;
    // What `apply_overrides()` put on the live tree, so `restore_overrides()` can take it off. An
    // override is an inline declaration, which `Node` keeps as a borrowed `string_view`: the names
    // and values live in `override_text_` (a deque, so growing it never moves a string a node
    // points at), and `original` borrows the IR's own text for the value the override replaced.
    struct AppliedOverride {
        std::uint64_t automation_id = 0;
        std::string_view property;
        bool had_original = false;
        std::string_view original;
    };
    std::vector<AppliedOverride> applied_overrides_;
    std::deque<std::string> override_text_;
    double next_timestamp_s_ = 0.0;
    bool built_ = false;
    std::string build_error_;
    // Set by a handler that changed state during a dispatch; consumed by the rebuild after it.
    bool dirty_ = false;
    // True while `settle_.run()` is on the stack. A settle round's relayout calls `rebuild()`,
    // which must not re-enter the loop: the outer call continues with the next round.
    bool in_settle_ = false;
};

} // namespace tessera::compiler::codegen::cli
