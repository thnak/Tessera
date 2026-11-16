# Dynamic children in the design-time preview

Status: accepted (issue #389). Scope: `tools/codegen/scene_build*.cpp`, the in-memory preview that
runs natively and as Wasm. The engine semantics this predicts are in
[compiler-node-state.md](compiler-node-state.md); the pipeline it sits in is
[compiler-wasm.md](compiler-wasm.md) section 7.

## 1. The question this answers

Before this change `build_scene()` refused any IR node carrying a `condition` or a `loop`:

```
TESSERA_SCENE_UNSUPPORTED: a conditional or repeated child (`node::4`) is not previewed yet
```

That made two ordinary shapes unpreviewable: `{open ? <A/> : <B/>}` (and `{open && <A/>}`) and
`{items.map((item) => <Row key={item} />)}`. Codegen already generated C++ for both, and the native
harness already ran it, so the preview was the only lane that gave up. The page showed "No scene".

The two questions that had to be answered before writing any code were: **which state does a
condition read?** and **what supplies a `map`'s array?** Both answers are below, and the rule that
decided them is that there must be **no parallel semantic path**: a preview that computed these itself
would be a second definition of the language, and a snapshot that agreed with the native harness only
until one of them was edited.

## 2. Which state a condition reads

The evaluator's scope, and nothing else. `build_scene()` seeds one `eval::Scope` and passes it to the
walk; it is filled from three sources, in this order of precedence:

1. the cells **carried** from the previous evaluation of an edited source (`scene_state.cpp`), which
   are already in the scope and are never recomputed;
2. every remaining declared `useState`/`useAnimatedValue` cell, evaluated from its initializer — the
   value the app holds before any handler runs;
3. whatever a **dispatched handler** assigned, since `PreviewSession::run_handler()` runs the IR body
   through the same evaluator against the same scope and then rebuilds.

A branch is chosen by `eval::Evaluator::evaluate(node.condition, scope)` and requiring a `bool`. That
is the *same* call the preview already used for text templates. There is no second evaluator, no
second copy of the state, and no preview-only notion of "current value".

**Can `carry` drive a branch?** Yes, and it is the mechanism the page uses. A carried
`state::open = true` is in the scope before the walk starts, so the condition reads it and the other
branch paints. Identity is what makes this safe: a cell is carried only when the component id, the
cell id and the IR type all match (`scene_state.hpp`), so an edit that renames a cell starts it from
its initializer instead of inheriting a stranger's value.

Within one live session a click does the same thing without `carry`: the handler mutates the scope and
`rebuild()` re-runs `build_scene()` against it. The condition is therefore re-evaluated on every
rebuild, which is what "a conditional re-evaluates when its state cell changes" means.

## 3. What supplies a `map`'s items

`loop.source`, resolved in that same scope. This is not a restriction the preview invented: IR v1
requires a loop source to be a **list reference** (`validate_node.cpp`,
`docs/specs/ir-v1-expr.md` section 5), and the lowering only accepts a `list<T>` state cell whose
initial is an array literal of literals or a list reference
(`hook_lowering.cpp`, `validate_list_initial`). So the source is always either
`state::<cell>` or `prop::<name>`, both of which the evaluator resolves to an `eval::Value::List`.

A consequence worth stating plainly: the preview cannot evaluate an *arbitrary expression* as a list
source, because the IR does not allow one. It also cannot **grow** a list — the IR has no `push`
action — so a list a handler would fill shows the list's initial (or carried) contents. That is what
the app shows before the handler runs, and it is stated rather than hidden.

Each item is built by the same `build_node()` with `loop.item` and `loop.index` bound in a scoped
overlay (`LoopScope`), so `{item.label}` or `{index}` inside the template resolves to the item's value.
The overlay restores any shadowed name afterwards, so a template cannot leak `item` into a later
sibling or permanently shadow a state cell of the same name.

### The bounded subset

| Bound | Value | What happens past it |
|---|---|---|
| items per repeated child | 1000 (`kMaxPreviewItems`) | hard failure naming the node and the count |
| list nesting | 1 level (a template inside a template is refused) | `TESSERA_SCENE_UNSUPPORTED` |
| condition type | must evaluate to `bool` | `TESSERA_SCENE_UNSUPPORTED`, with the actual type |
| list source type | must be a list | `TESSERA_SCENE_UNSUPPORTED`, with the actual type |
| tree size / depth | 4096 nodes, 32 levels (unchanged) | hard failure |

Every bound is a **hard failure**, never a truncation. A preview that painted the first 1000 items of
a 5000-item list would produce a tree that looks plausible and disagrees with the native build, and a
snapshot of it would compare equal to another truncated snapshot. The snapshot walk's rule is the same
(compiler-wasm.md section 7.2): a capped tree is an error, not a document.

## 4. Both branches are built

"Both branches are buildable" is satisfied by the IR's shape, not by a special case in the preview.
Lowering turns `{open ? <A/> : <B/>}` into **two sibling nodes** with complementary `condition`
expressions (`control_lowering.cpp`, `lower_ternary()`), and an `&&` form into one. They are ordinary
IR nodes at ordinary source positions, so the walk visits each in source order and builds the ones
whose condition holds. Somewhere else in the same source, the other branch is a node the same code
would build for the other state — there is no branch-specific path that could compile one and not the
other.

Only the selected branch is **in the tree**, which is what the app has too: generated code keeps both
subtrees alive in `branch_<n>_owned_` slots and attaches exactly one with `attach_child_at`
(`emit_conditional.cpp`). The keep-alive state of a hidden branch is a codegen property with no
scene-node counterpart, which is why taking the complement of the snapshot is right.

The observable consequence, and the one the test pins: a **hidden** branch containing a construct the
preview cannot build does not fail the shown tree; the same subtree, once selected, reports its
refusal. Without that pair the claim would be untestable, because a preview that built nothing at all
would also pass the first half.

## 5. Identity

A node's `uiId`/`automationId` come from `assign_identity()`, which already assigns ids across the
whole lowered IR including loop templates and both branches, so the preview reads `node.ui_id`
directly and needs no parallel id rule.

The one thing the preview reproduces rather than reads is an **item key**: `(template uiId, itemKey)`
is a list item's automation identity (`docs/specs/automation-ids-v1.md` section 1), and generated
code publishes the key on the item root only when its C++ form is an integer — `numeric_item_key()` in
`emit_list.cpp` handles the loop index, an `int` item and an integer literal, while a string key is
matched through a generated key table. The preview publishes `set_item_key()` under the same rule, so
the two sides agree on which items carry a numeric key. A **string** key therefore has no `itemKey` on
either side; that is a stated omission, not a divergence.

## 6. What is still not evaluated

Unchanged by this note: user component instances, `CustomElement`, virtual collections (`VirtualList`,
`VirtualGrid`), `Slot` positions, nested list templates, and inline-span `Text`. Animating enter/leave
is out of scope. Each is a named `TESSERA_SCENE_UNSUPPORTED` carrying the node id, so a page reports
*why* rather than showing an empty screen.

`scene_state.hpp`'s note that a list-item key extension to `(componentId, itemKey, cellId)` is deferred
**stays deferred**, and the reason is now checkable rather than assumed: preview items have no state
cells of their own, because the IR has no per-item `useState` and the carry key therefore has nothing
to key on. Per-item *component instances* are what would need it, and those are one of the constructs
this slice still refuses.
