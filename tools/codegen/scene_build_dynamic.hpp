// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/eval/evaluator.hpp"
#include "tessera/compiler/ir/node.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

// The two questions a dynamic child asks of state (issue #389), answered by the evaluator the rest
// of the preview already runs:
//
//   * a **conditional child** is a node carrying `condition`. Lowering emits one sibling node per
//     possible branch with complementary conditions
//     (src/compiler/lowering/src/control_lowering.cpp), so "which branch is shown" is exactly "does
//     this node's condition evaluate to true in the current scope". The scope is the one
//     `build_scene()` seeds: carried cells first, then the initializers, then whatever a dispatched
//     handler has since assigned. A branch is therefore driven by the same state a click changes,
//     with no second notion of state.
//   * a **repeated child** is a node carrying `loop` (ir-v1 section 5). Its `source` is a list
//     reference by validation, so the items are the list the evaluator returns for that reference
//     -- the declared cell's initial value (a `useState<T[]>(...)` array literal) or a carried
//     value on a later edit. The preview cannot *grow* a list (the IR has no `push` action), so a
//     list a handler would fill shows its initial contents, which is what the app shows before the
//     handler runs.
//
// **The bounded subset, stated rather than implied.** The preview evaluates exactly one list source
// per repeated child, at the initial (or carried) state, and at most kMaxPreviewItems items of it.
// Everything it cannot decide is a named `TESSERA_SCENE_UNSUPPORTED` carrying the IR node id, never
// a silently empty subtree: a preview that rendered nothing for a construct it did not understand
// would be indistinguishable from a screen that is genuinely empty.
namespace tessera::compiler::codegen::cli::detail {

// The item bound of one repeated child. The page's source is untrusted input, and an item table is
// the one place a preview tree could grow without a matching source line. A source over the bound
// is a hard failure, following the snapshot walk's rule that a capped tree is never truncated
// (docs/design/compiler-wasm.md section 7.2): a truncated preview would compare equal to a
// truncated snapshot and pass a gate it should fail.
inline constexpr std::size_t kMaxPreviewItems = 1000;

// Evaluates `node.condition` in `scope`. `show` is the branch decision. A non-bool condition is a
// failure, not a false one: the language types a condition as boolean and the evaluator's own
// `Conditional` kind rejects a non-bool test, so the preview reports rather than coerces. On
// failure `error` names the node and carries the evaluator's own code, so a condition the language
// cannot evaluate stays distinguishable from one that evaluates to false.
[[nodiscard]] bool evaluate_condition(eval::Evaluator& evaluator, const eval::Scope& scope,
                                      const ir::Node& node, bool& show, std::string& error);

// Evaluates the list a repeated child repeats over into `out`. On failure `error` names the node
// and says which of the three bounds was hit (no source, not a list, over kMaxPreviewItems).
[[nodiscard]] bool evaluate_items(eval::Evaluator& evaluator, const eval::Scope& scope,
                                  const ir::Node& node, eval::List& out, std::string& error);

// The item's automation key: `(uiId, itemKey)` is a list item's identity
// (docs/specs/automation-ids-v1.md section 1), and generated code publishes the key on the item
// root only when its C++ form is an integer (emit_list.cpp, `numeric_item_key`). `present` is false
// for a key of any other type, which is a string key: it has no `itemKey` on either side, so it is
// a stated omission rather than a failure. An evaluator *failure* is reported instead, because a
// key the language cannot evaluate is not the same thing as a string key.
[[nodiscard]] bool evaluate_item_key(eval::Evaluator& evaluator, const eval::Scope& scope,
                                     const ir::Node& node, bool& present, std::uint64_t& key,
                                     std::string& error);

} // namespace tessera::compiler::codegen::cli::detail
