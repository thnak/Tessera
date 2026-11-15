// SPDX-License-Identifier: MIT

#pragma once

#include "scene_state.hpp"
#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/state_snapshot.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
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
        // State carried from the previous evaluation of an edited source. `live_component_ids` is
        // every component id the new evaluation has, so a cell whose component disappeared is
        // reported as dropped instead of being offered to a component that took its place.
        const CarriedState* carried = nullptr;
        const std::vector<std::string>* live_component_ids = nullptr;
    };

    PreviewSession(const ir::IrModule& module, const ir::Component& component,
                   const ui::style::CompiledStyleSheet& sheet, const Options& options);
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

    [[nodiscard]] const eval::Scope& scope() const noexcept {
        return scope_;
    }
    [[nodiscard]] const ui::Node& root() const noexcept {
        return *root_;
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

private:
    // One installed scene binding: which IR handler a scene node's event must run. Kept in a
    // `unique_ptr` so the `void*` context handed to `Node::bind_event` keeps a stable address.
    struct HandlerSlot;

    void rebuild();
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
    eval::Scope scope_;
    StateCarryReport carry_report_;
    std::unique_ptr<ui::Node> root_;
    ui::InputRouter router_;
    eval::Evaluator evaluator_;
    std::vector<std::unique_ptr<HandlerSlot>> slots_;
    std::vector<std::string> ran_;
    std::vector<std::string> unrun_;
    double next_timestamp_s_ = 0.0;
    bool built_ = false;
    // Set by a handler that changed state during a dispatch; consumed by the rebuild after it.
    bool dirty_ = false;
};

} // namespace tessera::compiler::codegen::cli
