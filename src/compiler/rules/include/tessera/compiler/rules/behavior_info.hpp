// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/jsx/ast.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace tessera::compiler::rules {

// One behavior hook call after the form, target and option checks (ADR-064 item 7;
// docs/specs/behavior-hooks-v1.md sections 2 and 4).
//
// The analyzer records what the call wrote; lowering turns it into an IR `behaviors` entry. The
// manifest is the authority on what the hook accepts, so the analyzer compares against it rather
// than re-deriving the shape.
struct BehaviorInfo {
    std::size_t statement = 0; // index into the component body
    // The local name the call was bound to (`trap`), or empty for a bare statement, which lowering
    // names `beh::<hook>_<n>`.
    std::string name;
    std::string hook;     // the TSX hook name (`useFocusTrap`)
    std::string behavior; // the engine registry name (`focus_trap`)
    // The `useRef` names the call passes as targets, in manifest order.
    std::vector<std::string> targets;
    // The options object literal, or null when the hook takes none.
    const jsx::Expr* options = nullptr;
    // The output names and IR types the hook returns, in manifest order. Empty when it returns
    // nothing. The type is carried rather than assumed: a behavior may report a number or a string,
    // and lowering must not guess `bool`.
    std::vector<std::string> outputs;
    std::vector<std::string> output_types;
    bool exclusive = false;
};

} // namespace tessera::compiler::rules
