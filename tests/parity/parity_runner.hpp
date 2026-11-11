// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/ui/scene/state_snapshot.hpp"

#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Preview-parity runner (issue #7, docs/design/designer.md section 2). The designer runs a native
// handler's TSX preview body through the IR evaluator; production runs the developer's C++. The
// project does not compare the two (jsx-subset-v1 section 9.3), so this test-side runner does: it
// plays the same script of calls against both sides and compares every state cell after each step.
namespace tessera::parity {

namespace eval = tessera::compiler::eval;
namespace ir = tessera::compiler::ir;

// One call of the script: a handler id (`handler::2`, from the IR) or a native binding id
// (`bind::add`) and its arguments in parameter order.
struct Step {
    std::string target;
    std::vector<eval::Value> args;
};

// Script text, one step per line: `call <id> [arg...]`. Arguments are an int (`-3`), a float
// (`1.5`, `2e3`), `true`/`false` or a double-quoted string with `\"` and `\` escapes. Blank lines
// and lines starting with `#` are ignored.
[[nodiscard]] std::expected<std::vector<Step>, std::string> parse_script(std::string_view text);

// What the native side does for a step: invoke the generated component's handler (a click, a typed
// event or a direct call) and return its state afterwards, or why it could not. A null step asks
// for the state before any step.
using NativeSide = std::function<std::expected<ui::StateSnapshot, std::string>(const Step*)>;

// One difference between the two sides. `step` is the zero-based index in the script, or -1 for
// the initial state before any step. Values are `describe()`d text.
struct Mismatch {
    int step = -1;
    std::string target;
    std::string cell;
    std::string preview;
    std::string native;
};

struct Report {
    int steps_run = 0;
    std::vector<Mismatch> mismatches;
    // A problem with the run itself (unknown handler, wrong argument count, evaluator error).
    std::string error;

    [[nodiscard]] bool ok() const noexcept {
        return error.empty() && mismatches.empty();
    }
};

// The state the IR gives `component` before any step: every `useState` and `useAnimatedValue` cell
// with its initial expression evaluated.
[[nodiscard]] std::expected<eval::Scope, std::string> initial_scope(const ir::Component& component);

// Plays `script` on the IR side (the handler's actions, or the native binding's preview body) and
// on `native`, comparing the native snapshot with the IR state before the first step and after each
// one. It stops at the first step with a difference, so the report names where they diverge.
[[nodiscard]] Report run_parity(const ir::IrModule& module, const ir::Component& component,
                                const std::vector<Step>& script, const NativeSide& native);

// A report as text for a failing test: the step, the binding or handler id, each differing cell and
// both values.
[[nodiscard]] std::string format_report(const Report& report);

} // namespace tessera::parity
