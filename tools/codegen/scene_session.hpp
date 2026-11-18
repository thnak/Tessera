// SPDX-License-Identifier: MIT

#pragma once

#include "scene_layout.hpp"
#include "scene_state.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/state_snapshot.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cstddef>
#include <cstdint>
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
    // What the most recent `dispatch()` ran and could not run, in binding order. A canvas caller
    // reports these to the page so a handler this slice cannot execute is visible rather than
    // silent, the same split `PreviewClickResult` makes.
    [[nodiscard]] const std::vector<std::string>& last_ran() const noexcept {
        return ran_;
    }
    [[nodiscard]] const std::vector<std::string>& last_unrun() const noexcept {
        return unrun_;
    }

private:
    // One installed scene binding: which IR handler a scene node's event must run. Kept in a
    // `unique_ptr` so the `void*` context handed to `Node::bind_event` keeps a stable address.
    struct HandlerSlot;

    void rebuild();
    // Lays `root_` out against `layout_metrics_` and the current `sheet_`. One pass, no
    // re-evaluation: everything a size or posture change needs, and nothing else.
    [[nodiscard]] bool relayout();
    void bind_handler(std::uint64_t automation_id, const std::string& event,
                      const std::string& handler_id);
    static void on_scene_event(void* context, const ui::Event& event);
    void run_handler(const std::string& handler_id, const std::vector<ir::Action>* actions);
    [[nodiscard]] ui::Node* node_by_automation_id(std::uint64_t automation_id);
    void append_cell(ui::StateSnapshot& out, const std::string& id, const std::string& type) const;

    const ir::IrModule& module_;
    const ir::Component& component_;
    ui::style::CompiledStyleSheet sheet_;
    Options options_;
    // The media context `sheet_` was resolved against (issue #436), so a viewport change can ask
    // whether a `@media` answer moves before resolving anything.
    ui::style::MediaContext media_;
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
    std::vector<std::string> ran_;
    std::vector<std::string> unrun_;
    double next_timestamp_s_ = 0.0;
    bool built_ = false;
    // Set by a handler that changed state during a dispatch; consumed by the rebuild after it.
    bool dirty_ = false;
};

} // namespace tessera::compiler::codegen::cli
