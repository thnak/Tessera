# The canvas input session (#388)

The canvas module used to have one shape of call (the one-shot `tessera_canvas_frame`, since removed) that evaluated the
source, laid it out, painted it and returned. That is a *renderer*, and it cannot answer "what happens
when the pointer is over the button", because every call starts from an empty state and ends by
throwing the tree away.

This note records the engine half of the fix: one evaluation kept alive, pointer events routed into
it, and a frame painted only when something changed. The page half (the browser's pointer listeners
and its one-`requestAnimationFrame`-per-event scheduling) is **not** in this slice; see
[§6](#6-the-page-half-is-deferred) for why and for the exact call shape it should use.

The related records are [`compiler-wasm.md`](compiler-wasm.md) section 8 (the canvas and its
render-on-demand rule) and section 9 (the input session and the state-carry rule), and
[`input.md`](input.md) section 7 (hover/active in the router). This note supersedes neither: §8's
`tessera_input_json` is the per-call shape (§9), and the session here is the one that keeps an evaluation
alive between calls. Every export reads the **staged project** (`tessera_project_*`, see
[`compiler-wasm.md`](compiler-wasm.md) section 8.2), not a source argument.

## 1. Why a session, and what it owns

`prepare_scene()` is the right shape for a snapshot and the wrong shape for a canvas. The snapshot
view calls `tessera_input_json` with a *script* and gets the state cells back; the state belongs to a
tree that dies with the call. A canvas needs the opposite: the tree, its `ui::InputRouter` and its
state scope must outlive one pointer event and be visible to the *next* frame.

`cli::CanvasSceneSession` (`tools/codegen/scene_canvas_session.hpp`) is that lifetime. It holds:

| Member | Why it must live in the session |
|---|---|
| `EvaluatedScene evaluated_` | the lowered IR the scene borrows its element-kind strings from |
| `ui::style::CompiledStyleSheet sheet_` | the sheet the tree was laid out against; painting with another would draw a node at a size its box does not have |
| `PreviewSession session_` | the tree, the `InputRouter` (hover/active/focus/capture) and the evaluator's scope |
| `ui::DirtyPass dirty_` | consumed by `paint()`; reused so the frame path allocates nothing |
| `text_/file_/component_/width_/height_/dpi_scale_` | what the running evaluation was started from, so a Run of the identical source can be recognised |

Ownership order matters and is stated in the header: `evaluated_` is declared before `session_`, so
the IR outlives the tree that points into it.

## 2. How a pointer event reaches the style engine

This is the part worth not re-deriving. **No layer in this path writes a class name, and nothing
matches a selector by hand.**

```
tessera_canvas_pointer(kind, x, y, button)          wasm_canvas_entry.cpp
  → CanvasSceneSession::pointer(kind, x, y, button) scene_canvas_session.cpp
    → PreviewSession::dispatch(PreviewInput)        scene_session.cpp
      → ui::InputRouter::dispatch(root, InputEvent)  src/ui/scene/input_dispatch.cpp
        → Node::set_pseudo_state(PseudoState::Hover, true)      (hit target of a move)
        → Node::set_pseudo_state(PseudoState::Active, true)     (press target + ancestors)
          → Node marks Dirty::Style
    → a bound handler changed state → PreviewSession::rebuild()
  → the caller draws; paint() resolves style through
      style::SceneStyleTree::compute(sheet, node)
        → refreshes each node's selector view from Node::pseudo_state()
        → the cascade matches `Button:hover` / `Button:active`
```

Two consequences a reader should take away:

* The pseudo-classes are the router's, so they follow the same rules as native: only the primary
  pointer drives them, a captured pointer freezes the hover chain, a release clears `:active`, and a
  move that hits nothing clears the hover.
* `style::SceneStyleTree::compute()` re-reads the state from the node on every call (the comment on
  `SceneStyleTree` says so), which is why a session captured before a press still resolves the right
  colour afterwards - the test in `tests/unit/scene_canvas_pointer_tests.cpp` depends on exactly
  that.

The preview stylesheet (`preview_stylesheet()` in `tools/codegen/scene_prepare.cpp`) gained three
rules for this: `Button:hover`, `Button:active`, and a hover tint for the two text fields. They are
the only pseudo-class rules in that sheet, and they match nothing when no pointer is over the scene,
so every existing golden resolves to the same colours it did before.

## 3. One frame per change, and nothing for a static scene

`CanvasSceneSession::needs_frame()` answers from the scene's own dirty domains
(`Style | Layout | Paint | Render`), not from a flag the page sets:

* a Run is a change (one frame);
* a dispatch that moved a pseudo-class sets `Dirty::Style` (one frame);
* a handler that changed state rebuilds the tree, which re-marks everything (one frame);
* a move that stays inside one node, a repeated leave, or a second `paint()` with no event in
  between, moves nothing - **zero** frames.

`paint()` clears those domains after a successful frame, which is what makes the next `needs_frame()`
answer false. A caller therefore draws at most one frame per event and never needs a loop; there is no
`requestAnimationFrame` re-arm anywhere in the engine half.

## 4. Coordinates: CSS pixels, logical pixels, and the DPR trap

The C ABI takes **logical layout-viewport pixels**, the same space the layout engine used. The page
owns the conversion, because only it can read the canvas' CSS box.

The rule, which is also a comment on `tessera_canvas_pointer`:

```
const rect = canvas.getBoundingClientRect();     // the CSS box the user sees
const x = (event.clientX - rect.left) * (logicalWidth / rect.width);
```

Divide by the canvas' **CSS-to-logical ratio**, never by `devicePixelRatio`.

Worked case: a 320×240 logical viewport at DPR 2 gives a 640×480 backing store and a 320×240 CSS box.
A pointer at CSS (10, 10) is logical (10, 10), and `640/320 = 2` is *not* a factor anywhere in the
mapping. `(clientX - rect.left) / dpr` would hand the module 5 and hit-test half a button away - which
is the bug this note exists to prevent. Dividing by DPR is only right when the page has set no CSS
size at all and the canvas is displayed at its natural backing-store size, which is not the layout
this module is given.

## 5. The C ABI added

| Export | Contract |
|---|---|
| `tessera_canvas_apply(component, fold, width, height, dpi)` | evaluate the staged project, keep it (carrying state across an edit); returns a report with `error` set when it produced no scene. An apply of the identical project reuses the evaluation instead of rebuilding it |
| `tessera_canvas_pointer(kind, x, y, button)` | `kind` is `"move" \| "down" \| "up" \| "cancel"`; coordinates are logical pixels; returns `{"pointerVersion":1,"dispatched":…,"changed":…,"targetId":"…","hoveredId":"…","activeId":"…","ran":[…],"unrun":[…]}`, all ids as decimal **strings** |
| `tessera_canvas_pick(x, y)` | what is under a point, for the picker overlay ([§5.1](#51-the-picker-query-issue-171)); coordinates are logical pixels; returns `{"pickVersion":1,"found":…,"automationId":"…","elementKind":"…","box":{"x":…,"y":…,"width":…,"height":…}}`. A query: it dispatches nothing and owes no frame |
| `tessera_canvas_draw(width, height, canvas_view)` | paint the session's current tree into the canvas view; never re-evaluates |
| `tessera_canvas_set_fold(posture)` | re-lay the RUNNING tree out at a simulated fold posture (`flat`, `book`, `tabletop`), one pass, no re-evaluation (issue #408, [§6.1](../specs/designer-studio-v1.md)) |
| `tessera_canvas_set_viewport(width, height, dpi)` | re-lay the RUNNING tree out at a new logical viewport and DPR, one pass, no re-evaluation; returns `{"viewportVersion":1,"changed":…,"width":…,"height":…,"dpr":…,"cascadeGeneration":…,"error":""}` (issue #436) |
| `tessera_canvas_reset()` | release the session |

`changed` is the page's whole decision: schedule one frame when true, none when false. An unknown
`kind` and a secondary `button` answer a well-formed report with `dispatched:false, changed:false`, so
a page typo costs no frame and raises nothing across the FFI boundary.

**Why the size is its own export (issue #436).** A viewport change must not re-evaluate the source:
the state cells, the hover and the focus belong to the running evaluation. `set_viewport` therefore
does what `set_fold` does — one layout pass inside the call, on the same tree — and, unlike the
posture, it can also move the CASCADE, because the logical size is the `@media` context. It
re-resolves the prepared sheet only when a breakpoint is actually crossed, and `cascadeGeneration` in
its report (and in the frame report's `cascade` field) is the count that makes the difference
observable: a resize inside one breakpoint moves the boxes and leaves the number alone. The rule is
[`responsive.md`](responsive.md) section 2.1; the labels of both fields (`changed`, `frames`,
`cascade`) are what `scripts/ci/preview_browser_viewport.mjs` asserts in a real browser.

### 5.1 The picker query (issue #171)

The picker overlay is the first slice of #171: while it is armed, moving the pointer over the canvas
outlines the node under it and shows that node's identity in a small readout. It needs one fact from
the engine — *which node is at this point, and where is its box* — and it needs that fact **without
changing the scene**.

**Why a query and not a pointer kind.** `tessera_canvas_pointer` dispatches: it sets `:hover`, may run
a handler, and answers `changed` so the page draws a frame. A picker must do none of that. Arming it
and moving over a button has to leave the button at its resting colour, run nothing, and cost zero
frames — otherwise the overlay would be an animation loop that repaints the canvas on every pointer
move, which is exactly what §3 exists to prevent. So `tessera_canvas_pick` is a separate export that
reads the tree and returns a value. `CanvasSceneSession::pick()` sets no pseudo-class, marks no dirty
domain and allocates nothing beyond the two borrowed strings it copies out.

**Why it is not a second hit test.** `CanvasSceneSession::pick()` calls `Node::hit_test_deep`, the
function `ui::InputRouter` calls to decide a pointer target. So the node the overlay outlines is the
node a press at the same point would reach, including `pointer-events: none`, scrollport clipping,
z-order and `visibility: hidden`. A hand-written hit test would be a second copy of those rules, and
the first time the two disagreed the overlay would outline one node while a click activated another.

**The box is in logical pixels.** `box` is `Node::world_bounds()`, in the same space `pointer()` takes
its coordinates in, so the page converts it with the CSS-to-logical ratio of §4 — the same factor it
divides the pointer by, never the DPR. One conversion in each direction is what keeps the outline
under the pointer that produced it.

**A miss is a real answer.** `found:false` (with a zero box) means the point is over no node: outside
the viewport, or over a gap the tree does not cover. The page hides the overlay rather than keeping
the last outline, which would claim the pointer is still over the node it left. An unstarted session
answers the same way, so a page that arms the picker before the first Run costs no exception at the
FFI boundary.

**The page half.** `designer/web/src/views/canvas-picker.ts` owns the overlay: a `div` positioned over
the canvas, `pointer-events: none` so it never intercepts a move or a press, with an outline and a
readout built into it. It is **off by default** and armed by the `Picker` toggle in the Canvas pane's
header (`aria-pressed` carries the state). The overlay is a page element rather than engine paint for
two reasons: an engine-drawn outline would be part of the frame, so it would have to be repainted on
every move and would contaminate the pixels a golden compares; and ADR-054 decision 3 says the engine
draws the previewed app and nothing else. The outline is re-asked after a frame, a viewport change or
a restart, because the boxes it was drawn for may have moved.

## 6. The page half is deferred

ADR-054 moves the studio front end to React 19 + Vite (issue #422), and the owner's instruction is
that #422 blocks #388's page work. So `designer/web/*.js` is deliberately **untouched** by this
slice, and no `PAGE_FILES` entry or budget re-record was needed.

A future React view needs no re-derivation. Its pointer handling is:

```js
// width/height below are the values passed to tessera_canvas_apply for the running session.
function toLogical(event, canvas, logicalWidth) {
  const rect = canvas.getBoundingClientRect();
  return {
    x: (event.clientX - rect.left) * (logicalWidth / rect.width),
    y: (event.clientY - rect.top) * (logicalHeight / rect.height),
  };
}

canvas.addEventListener('pointermove', (event) => {
  const { x, y } = toLogical(event, canvas, logicalWidth);
  if (JSON.parse(callExport(module, '_tessera_canvas_pointer', ['move'], [0], [x, y, 0])).changed) {
    requestFrame();          // one frame, coalesced by the existing framePending guard
  }
});
// pointerdown → 'down' with event.button; pointerup → 'up'; pointercancel → 'cancel'.
// pointerleave → 'move' at (-1, -1): a move that hits nothing clears the hover, which is exactly
// what the router does for a real pointer leaving the scene.
```

`requestFrame()` is the same `framePending` + `requestAnimationFrame` shape `view-canvas.js` already
has; the only change is that the *reason* to call it now comes from `changed` rather than from a
keystroke in the editor.

## 7. Verification and status

Verified on this Windows host (2026-11-16):

* `cmake --preset vs2026 && cmake --build --preset vs2026-release --parallel 12`: exit 0.
* `ctest --preset vs2026-release --parallel 12`: exit 0, 0 failures. The two new cases are
  `scene_canvas_session` and `scene_canvas_pointer`; `scene_frame` and `scene_input` still pass.
* `cmake --build build/wasm-webgpu --target tessera_canvas_wasm` (emsdk 7.0.0 from `C:\emsdk392`):
  exit 0, and the linked `tessera_canvas.js` contains `_tessera_canvas_apply`, `_tessera_canvas_pointer`,
  `_tessera_canvas_draw` and `_tessera_canvas_reset` in its export table. This is a *build* fact, not
  a runtime one.
* `python -B scripts/check_source_size.py`: 0 errors. `python -B scripts/check_public_api.py --check`:
  0 added, 0 removed - nothing under `include/tessera/` changed, so the public API is untouched.
* `python -B scripts/gates.py`: 10 of 11 gates pass. `docs`, `source-size`, `public-api`, `pins`,
  `version-policy` and `memory-budget` among them. The one failure is the pre-existing
  `format-version` (no `clang-format` on this host).
* `node scripts/ci/preview_page_checks.mjs`: 20 checks pass. `node
  scripts/ci/preview_share_roundtrip.mjs`: 19 pass. `python -B scripts/check_preview_budget.py
  --check`: 0 errors, `download_gzip_bytes` measured 436,792 of its 436,794 budget - the page assets
  are unchanged, so the budget did not move.
* Mutation check on the frame accounting: making `needs_frame()` return true unconditionally fails
  **14** assertions in `scene_canvas_session_tests.cpp` and **10** in
  `scene_canvas_pointer_tests.cpp`, so the zero-frame and one-frame assertions are not vacuous.

**The picker slice (issue #171, 2026-11-18).** Added `tessera_canvas_pick` and its report, the
`scene_canvas_pick` ctest case, and the page's overlay. Verified on this Windows host:

* `cmake --build build\vs2026 --config Release --parallel 12 --target scene_canvas_pick_tests
  scene_canvas_pointer_tests scene_canvas_session_tests`: exit 0, `/WX` clean.
* The three executables pass: `scene_canvas_pick: ok`, `scene_canvas_pointer: ok`,
  `scene_canvas_session: ok`. The new case asserts the picked node and its box against the tree's own
  `automation_id()`/`world_bounds()`, a miss outside every node, and that ten picks over a button
  leave `hovered()` null, the resting colour unchanged and the frame count at 1.
* `cmake --build build/wasm-webgpu --target tessera_canvas_wasm`: exit 0, and the linked
  `tessera_canvas.js` contains `_tessera_canvas_pick` in its export table. A *build* fact, not a
  runtime one.
* `npm run build --prefix designer/web` (typecheck + site + SSR chrome): exit 0.
  `node scripts/ci/preview_page_checks.mjs`: all pass, including the canvas pane's 19 ids (the picker
  toggle and overlay are two of them).

**Not verified: the overlay in a browser.** Nothing here has been run against a real WebGPU device or
a real pointer, so "the outline lands under the pointer" is reasoned from the shared CSS-to-logical
conversion rather than observed. The engine half is proven on the null RHI; the page half is proven
to build, typecheck and render its ids. A browser probe is the follow-up, and it needs the same
Playwright-class dependency `compiler-wasm.md` section 8.3 records as missing.

## 8. Files

| Path | Role |
|---|---|
| `tools/codegen/scene_canvas_session.{hpp,cpp}` | the persistent session: evaluation, pointer dispatch, frame accounting, and the picker's `pick()` query |
| `tools/codegen/scene_frame.{hpp,cpp}` | `scene_frame_paint_tree()` - paint a caller-owned tree; `scene_frame_paint()` is now its wrapper |
| `tools/codegen/canvas_report.{hpp,cpp}` | `canvas_pointer_report_json()` and `canvas_pick_report_json()` beside the existing frame report (host-compilable) |
| `tools/codegen/wasm_canvas_entry.cpp` | the C ABI: `tessera_canvas_apply/pointer/pick/draw/reset`, with the coordinate rule on the exports |
| `tools/codegen/scene_prepare.cpp` | `preview_stylesheet()` gains `Button:hover`, `Button:active`, `TextInput:hover`, `TextArea:hover` |
| `tools/codegen/scene_session.{hpp,cpp}` | `last_ran()`/`last_unrun()`/`router()` accessors, so the session can report what a dispatch did |
| `tests/unit/scene_canvas_session_tests.cpp` | lifecycle + the frame contract (a static scene draws zero) |
| `tests/unit/scene_canvas_pointer_tests.cpp` | the pointer path into the style engine, click synthesis, ignored buttons, cancel |
| `tests/unit/scene_canvas_pick_tests.cpp` | the picker query: the right node and box, a miss, and that a pick changes nothing |
| `tests/unit/scene_canvas_test_support.hpp` | the shared sources, tree helpers and the on-demand `CanvasHarness` |
| `designer/web/src/views/canvas-picker.ts` | the page's overlay: the outline, the readout, the toggle and the CSS-to-logical conversion |
| `designer/web/src/regions/CanvasPane.tsx` | the overlay element (`pointer-events: none`) and the `Picker` toggle |
| `src/compiler/codegen/CMakeLists.txt` | the module-local build wiring (see the answer accompanying this landing) |
