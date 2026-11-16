// SPDX-License-Identifier: MIT

// IR -> live scene tree for the in-memory preview (issue #166): the entry point that seeds state
// and then hands the walk to `scene_build_walk.cpp` / `scene_build_children.cpp`.
//
// **Where a branch's condition and a list's items come from (issue #389).** Both come from the
// scope this function seeds, and from nothing else:
//
//   1. the cells carried from the previous evaluation of an edited source (`scene_state.cpp`, the
//      `carry` rule) are already in `scope` and win;
//   2. every remaining declared `useState`/`useAnimatedValue` cell is evaluated from its
//   initializer,
//      which is the value the app has before any handler runs;
//   3. a handler the session dispatches assigns into this same `scope` (`scene_session.cpp`), so
//   the
//      next rebuild sees the new value.
//
// A condition is then a `ir::Expr` evaluated through `eval::Evaluator::evaluate()` against that
// scope, and a list's items are the list the loop's `source` reference resolves to in it. That is
// one evaluator and one notion of state for the preview's text, its branches and its lists -- the
// preview does not grow a second semantic path, so what it shows keeps predicting what the
// generated C++ would show for the same state.
//
// The bounded subset is stated in `scene_build_dynamic.hpp`: one list source per repeated child,
// read at the seeded state, at most kMaxPreviewItems items; a condition must be a bool; anything
// else is a named `TESSERA_SCENE_UNSUPPORTED` carrying the node id.

#include "scene_build.hpp"
#include "scene_build_internal.hpp"

#include <string>

namespace tessera::compiler::codegen::cli {
namespace {

namespace eval = tessera::compiler::eval;
namespace ir = tessera::compiler::ir;

} // namespace

SceneBuildResult build_scene(const ir::Component& component, ui::Node& root, eval::Scope& scope) {
    SceneBuildResult result;
    // The state cells first, in declaration order, from their initializers: a node expression reads
    // them through `state::<name>` (ir-v1 section 4), so the scope must exist before the walk. This
    // is the evaluator's `initial_scope()` rule (tests/parity/parity_runner.cpp), kept in step with
    // it by construction: both build the scope from the declared cells and their initial
    // expressions.
    eval::Evaluator evaluator;
    const eval::Scope empty;
    // The state cells of a carried edit are already in `scope` (`scene_state.cpp`); every other
    // cell is evaluated from its initializer. The presence test is the whole merge rule: a carried
    // value is never recomputed, and an initializer never overwrites one. This keeps the preview's
    // "state survives an edit" behaviour in one place instead of re-deriving it per caller.
    const auto add = [&](const std::string& id, const std::string& type,
                         const ir::ExprPtr& initial) -> bool {
        if (scope.find(id) != scope.end()) {
            return true; // carried across the edit
        }
        if (!initial) {
            scope[id] = eval::Value::null();
            return true;
        }
        auto value = evaluator.evaluate(*initial, empty);
        if (!value) {
            result.error = id + ": " + value.error().code + ": " + value.error().message;
            return false;
        }
        // A float-typed cell holds a double even when its initial is written as an integer, as the
        // generated field does (`float` and `length` both map to `double`).
        if ((type == "float" || type == "length") && value->as_int() != nullptr) {
            *value = eval::Value::of_float(value->number());
        }
        scope[id] = std::move(*value);
        return true;
    };
    for (const ir::State& state : component.state) {
        if (!add(state.id, state.type, state.initial)) {
            return result;
        }
    }
    for (const ir::AnimatedValue& value : component.animated_values) {
        if (!add(value.id, value.type, value.initial)) {
            return result;
        }
    }
    internal::Builder builder(root, scope, result);
    // A failed build leaves `error` set for the caller; the tree is discarded, because a partially
    // built tree would be a snapshot of a program that does not exist.
    (void)builder.build(component);
    return result;
}

} // namespace tessera::compiler::codegen::cli
