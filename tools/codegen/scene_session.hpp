// SPDX-License-Identifier: MIT

#pragma once

#include "scene_build.hpp"
#include "scene_layout.hpp"
#include "scene_override.hpp"
#include "scene_pending.hpp"
#include "scene_state.hpp"
#include "scene_stylesheet.hpp"
#include "scene_text_field.hpp"
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
        // Text for the focused field (issue #601): a commit, and the IME preedit lifecycle.
        Text,
        CompositionStart,
        CompositionUpdate,
        CompositionEnd,
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
    std::string text; // UTF-8 payload of a Text/Composition kind, at most `ui::kMaxEditTextBytes`
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

    // The focused `TextInput`/`TextArea` (issue #601). Text and editing keys reach it through
    // `dispatch()`: the router delivers them to the node, whose `TextInputRuntime` edits the buffer
    // and emits `TextChange`, which runs the `onChange` body with a `TextChangeEvent`.
    [[nodiscard]] PreviewTextField focused_text_field() const;
    // The focused field's selected text (copy and cut read it), empty when nothing is selected.
    [[nodiscard]] std::string focused_text_selection() const;
    // True when the last `dispatch()` was consumed by a text field (the page may preventDefault).
    [[nodiscard]] bool text_consumed() const noexcept {
        return text_consumed_;
    }
    // Publishes every field's caret, selection and preedit into its paint data (idempotent; an
    // unchanged decoration marks nothing dirty); `dispatch()` and `rebuild()` call it.
    void sync_text_fields();
    // A press or drag (`extend`) in the focused field; `clicks` 2 selects a word, 3+ the value.
    bool move_text_caret(std::size_t byte_offset, bool extend, std::uint32_t clicks = 1);

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

    // Resizes the RUNNING tree in place (issue #436): the viewport and DPI scale are layout inputs
    // like the posture, so a resize re-lays the tree out and re-resolves `@media` (only when the
    // move can change an answer, ADR-048; counted by `cascade_generation()`) without evaluating the
    // source again: hover, focus, state and capture survive. False when nothing moved.
    bool set_viewport(float width, float height, float dpi_scale);

    // One pass for both layout inputs; the two setters above are its narrow spellings.
    bool relayout_at(float width, float height, float dpi_scale, tessera::hal::FoldPosture posture);

    // The same pass with the skin's safe area (issue #171): size, DPI, posture, insets and cutouts
    // move together in ONE layout pass; insets are part of the "did anything move?" test.
    bool relayout_at(float width, float height, float dpi_scale, tessera::hal::FoldPosture posture,
                     const tessera::hal::WindowInsets& insets,
                     const tessera::hal::CutoutInfo& cutouts);

    // Brings the live tree in line with the override layer (designer-protocol-v1 sections 9.2-9.4):
    // overrides are taken off, the layer's entries put on, and the tree laid out again - a restyle,
    // never a re-evaluation. A frame is owed afterwards. False when the session built no tree.
    bool resync_overrides();

    // How many times the cascade has been resolved: 1 at construction, +1 per breakpoint crossing
    // in `set_viewport` (issue #436).
    [[nodiscard]] std::uint64_t cascade_generation() const noexcept {
        return cascade_generation_;
    }

    // The sheet the tree is CURRENTLY laid out with; after `set_viewport` crosses a breakpoint it
    // is a different sheet, so a painter reads it here rather than keeping a copy.
    [[nodiscard]] const ui::style::CompiledStyleSheet& sheet() const noexcept {
        return sheet_;
    }

    // The ADR-041 settle loop (issue #693), shared with `AppHost` and driven from `rebuild()` and
    // `relayout()`, so a crossed `@container` condition has re-styled before the caller paints.
    [[nodiscard]] const ui::layout::LayoutSettle& settle() const noexcept {
        return settle_;
    }
    // The settle counters (rounds, engine rounds, deferred observations, round-limit hits).
    [[nodiscard]] const ui::layout::SettleTotals& settle_totals() const noexcept {
        return settle_.totals();
    }

    // The `useBreakpoint()` consumers (issue #692), pushed from `set_viewport` as `AppHost` does.
    [[nodiscard]] ui::responsive::EnvironmentSet& environment_values() noexcept {
        return environment_;
    }
    // The size class in force, from the media context the `@media` rules resolve against.
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
    // The live tree for the animation driver (#603).
    [[nodiscard]] ui::Node& mutable_root() noexcept {
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
    // The preview does not run the developer's C++, so it must run the engine behavior itself, from
    // the same IR entry and registry as the generated base.
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

    // The pending UI of the preview (issue #657, pending-ui-v1.md section 10): drives one resource
    // double and rebuilds the tree, so the page shows the fallback, the content or the pending
    // state the boundary rule gives. `ok` is false for a component with no `useResource`.
    [[nodiscard]] PendingControlResult control_resource(const PendingControl& control);
    [[nodiscard]] PendingReport pending_report() const;

private:
    // One installed scene binding: which IR handler a scene node's event must run. Kept in a
    // `unique_ptr` so the `void*` context handed to `Node::bind_event` keeps a stable address.
    struct HandlerSlot;
    struct CallContext;
    // One installed `onSizeChange` observation (ADR-041 item 3, issue #694). The preview is an IR
    // evaluator, so it cannot run the developer's C++ thunk; it runs the same IR body the generated
    // base would, with a `SizeChangeEvent` record bound to the handler's parameter. The slot is
    // kept in a `unique_ptr` for the same reason `HandlerSlot` is: `LayoutObservation` holds the
    // context pointer for its whole life.
    struct SizeSlot;
    struct TextField;
    void attach_text_fields(const SceneBuildResult& built);
    void detach_text_fields();
    // The bound scene events of a field: the six text/key events feed its runtime; `TextChange`
    // runs the `onChange` body.
    static void on_text_event(void* context, const ui::Event& event);
    static void on_text_change(void* context, const ui::Event& event);
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
                      const std::string& handler_id, std::size_t frame);
    // Runs `actions` against the scope of component frame `frame` (0 is the root component). A
    // `callProp` in them runs the caller's handler in the caller's frame (`on_prop_call`).
    // `scope` replaces the frame's scope for this run (a handler whose parameter is bound).
    [[nodiscard]] bool run_actions(std::size_t frame, const std::vector<ir::Action>& actions,
                                   eval::Scope* scope = nullptr);
    static void on_prop_call(void* context, const std::string& target, bool has_payload);
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
    // Writes the outputs into the scope (`beh::<name>.<output>`); true when any value changed.
    bool sync_behavior_outputs();
    // Rebuilds (bounded) until the tree's outputs match the live behaviors; replays the pointer.
    void settle_behavior_outputs();
    static void on_scene_event(void* context, const ui::Event& event);
    void run_handler(const std::string& handler_id, const std::vector<ir::Action>* actions,
                     std::size_t frame);
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
    // Null unless the component declares a `useResource`; owns the resource doubles.
    std::unique_ptr<PendingPreview> pending_;
    // Component instances (issue #594): their state cells persist here across rebuilds, and
    // `frames_` is the last build's frame table, which handlers index by frame number. Frame 0 is
    // the root component and points at `scope_`.
    InstanceScopes instance_scopes_;
    std::vector<SceneInstanceFrame> frames_;
    // The metadata strings the last build evaluated, which the tree borrows.
    std::deque<std::string> metadata_strings_;
    CallContext* active_call_ = nullptr;
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
    // One per text field; survives `rebuild()` (matched by automation id).
    std::vector<std::unique_ptr<TextField>> text_fields_;
    bool text_consumed_ = false;
    // One behavior instance per IR `behaviors` entry, in declaration order (ADR-064 item 7). The
    // evaluated args are kept alongside because `refresh` re-passes them.
    struct BehaviorSlot {
        std::string name; // the engine registry name
        ui::behavior::Instance instance;
        std::vector<ui::behavior::ArgValue> args;
        // The `ref::` ids this behavior targets, in manifest order, and the scene node each one
        // resolved to. A node id of 0 means the ref did not resolve (a branch that did not take).
        std::vector<std::uint64_t> target_nodes;
    };
    std::vector<BehaviorSlot> behaviors_;
    // Last pointer position, replayed after an output-driven rebuild.
    bool has_pointer_ = false;
    float pointer_x_ = 0.0f;
    float pointer_y_ = 0.0f;
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
