# The canvas input session (#388)

The canvas module used to have one shape of call (the one-shot `tessera_canvas_frame`, since removed) that evaluated the
source, laid it out, painted it and returned. That is a *renderer*, and it cannot answer "what happens
when the pointer is over the button", because every call starts from an empty state and ends by
throwing the tree away.

This note records the engine half of the fix: one evaluation kept alive, pointer events routed into
it, and a frame painted only when something changed. The page half (the browser's pointer listeners
and its one-`requestAnimationFrame`-per-event scheduling) was not part of this engine slice and
landed later; [§6](#6-the-page-half-landed) records what it does and the call shape it uses.

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
tessera_canvas_pointer(kind, x, y, button, mods, clicks)  wasm_canvas_entry.cpp
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
| `tessera_canvas_pointer(kind, x, y, button, modifiers, clicks)` | `modifiers` bit 0 is Shift and `clicks` the consecutive press count (both optional: 0 means none and 1); they only drive text selection ([`canvas-text-input.md`](canvas-text-input.md)). `kind` is `"move" \| "down" \| "up" \| "cancel"`; coordinates are logical pixels; returns `{"pointerVersion":1,"dispatched":…,"changed":…,"targetId":"…","hoveredId":"…","activeId":"…","ran":[…],"unrun":[…]}`, all ids as decimal **strings** |
| `tessera_canvas_pick(x, y)` | what is under a point, for the picker overlay ([§5.1](#51-the-picker-query-issue-171)); coordinates are logical pixels; returns `{"pickVersion":1,"found":…,"automationId":"…","elementKind":"…","box":{"x":…,"y":…,"width":…,"height":…}}`. A query: it dispatches nothing and owes no frame |
| `tessera_canvas_style(automationId)` | the style inspector's query ([§5.2](#52-the-style-inspector-query-issue-171)): the computed style, the matched rules with their provenance, and the box of one node. A query: it dispatches nothing and owes no frame |
| `tessera_canvas_overlay()` | the debug overlay's geometry (issue #433): the hinge band and the safe-area, keyboard, gesture and cutout rectangles the RUNNING tree was laid out against, as `{"overlayVersion":1,"width":…,"height":…,"foldState":"…","foldOrientation":"…","foldOcclusion":"…","separating":…,"hingeAngle":…,"hinge":{…},"insets":{…},"ime":{…},"gestures":{…},"cutouts":[…]}`. Serialized from the session's own `WindowMetrics` (the snapshot the layout consumed), so a page that scaled it can never place a band the layout did not use. A query: it dispatches nothing and owes no frame, which is why arming the overlay presents nothing |
| `tessera_canvas_key(kind, key, modifiers, repeat)` | keyboard (issue #388): `kind` is `"down" \| "up" \| "blur"`; `key` is a W3C `KeyboardEvent.key`; `modifiers` a Shift 1 / Control 2 / Alt 4 / Meta 8 mask. Returns the pointer report plus `focusedId`, `focusVisible`, `focusMoved`, `handled` (the page calls `preventDefault`) and `left` (Tab walked off the end of the tab order: focus is released and the browser carries on, so the canvas is never a keyboard trap). Enter activates on key-down, Space on key-up; the `:focus-visible` ring shows only after keyboard traversal |
| `tessera_canvas_text(kind, text)` | typed text for the focused `TextInput`/`TextArea` (issue #601, [`canvas-text-input.md`](canvas-text-input.md)): `kind` is `"commit"` (typed or pasted text, an IME result), `"start" \| "update" \| "end"` (the IME preedit) or `"copy" \| "cut"` (the report's `text` is the selection). Returns the key report; `handled` says the field took it. Every pointer and key report also carries `textFocus`, `caret`, `selectionStart`, `selectionEnd`, `composing`, `multiline`, `fieldBox` and `caretBox` (logical pixels; the IME window sits at the caret, and a press in the field places it, a drag selects) while a text field holds focus, and none of them otherwise |
| `tessera_canvas_tick(now_ms, reduced_motion)` | advance the session's animation clock to the page's `requestAnimationFrame` timestamp and sync style, so transitions and `@keyframes` start and move (issue #603, [§5.7](#57-animation-issue-603)); returns `{"tickVersion":1,"running":…,"dtMs":…,"samples":…,"reducedMotion":…}`. Called before each draw |
| `tessera_canvas_draw(width, height, canvas_view)` | paint the session's current tree into the canvas view; never re-evaluates. The report carries `animating`, the keep-alive of [§5.7](#57-animation-issue-603). `width`/`height` are the **physical** extent the page sized the backing store to, and the paint pass composes the session's own DPI scale onto every position (issue #599, [§5.5](#55-the-device-scale-issue-599)) |
| `tessera_canvas_set_fold(posture)` | re-lay the RUNNING tree out at a simulated fold posture (`flat`, `book`, `tabletop`), one pass, no re-evaluation (issue #408, [§6.1](../specs/designer-studio-v1.md)) |
| `tessera_canvas_set_viewport(width, height, dpi)` | re-lay the RUNNING tree out at a new logical viewport and DPR, one pass, no re-evaluation; returns `{"viewportVersion":1,"changed":…,"width":…,"height":…,"dpr":…,"clamped":…,"cascadeGeneration":…,"error":""}` (issue #436). The request is held to the viewport limits (issue #690, [§5.4](#54-the-viewport-limits-issue-690)): an over-limit value is clamped and reported by `clamped`, a non-finite or non-positive one is refused with `error` and the picture stays |
| `tessera_canvas_platform_event(type, value, timestampNs)` | one normalized hardware button or environment event of a device profile (issue #446, [protocol §5.1](../specs/designer-protocol-v1.md#51-platformevent)); returns the reply document (`handled`, `appClosed`, `resumed`, `state`, `dispatch`, `sequence`, `timestampNs`, `changed`, ...). The event goes through `hal::SyntheticEventInjector` and `PlatformEventDriver` (`scene_platform.cpp`), the code the native scripted run executes. A `relaunch` reply ends the evaluation, so the page's next `tessera_canvas_apply` is the cold start. `timestampNs` is a double (a 64-bit integer crosses the ABI as a BigInt); negative means "now" |
| `tessera_canvas_resource(action, resource, text, number, epoch)` | drive one resource double of the RUNNING session (issue #657, [pending UI](../specs/pending-ui-v1.md) section 10, [protocol §5](../specs/designer-protocol-v1.md#5-messages-host-to-viewer), `resource.control`): `action` is `fetch`, `resolve`, `fail` or `status` (a read that applies nothing), `resource` is `res::<name>` or `<name>`, `text`/`number` carry the value of a string- or number-typed resource (and `text` the `fail` message), `epoch` 0 is the current one and a stale one is dropped. Every resource opens with a fetch in flight, so the preview opens on the Suspense fallback; a transition-marked handler then shows `isPending`. Returns `{"pendingVersion":1,"ok":…,"accepted":…,"changed":…,"isPending":…,"resources":[…],"boundaries":[…],"error":""}`; this is the call the Studio mock worker (ADR-055) makes for a fake resource |
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

### 5.2 The style inspector query (issue #171)

The style inspector is the second slice of #171, and the read half of
[`designer-protocol-v1.md`](../specs/designer-protocol-v1.md) section 9.1. With it armed, clicking a
node in the canvas shows that node's **computed style**, the **rules that matched it** (highest
precedence first, each with its origin, selector text, specificity, source span and declarations),
and its **box**. `style.override.set` is a later slice; the answer's `overrides` list is present and
empty until it lands. `source.patch` is section 5.3 below.

**Why the engine answers it.** Every field is a fact only the cascade has. `computed` is the
resolved style after inheritance and `var()`; `matched` is the selector index's own answer with the
specificity it matched at; `overridden` is the cascade's own comparison. A page that recomputed any
of it would be a second cascade, and the first time the two disagreed the inspector would describe a
style the canvas is not painting. So `CanvasSceneSession::inspect()` resolves through
`style::SceneStyleTree::compute()` - the call the paint pass makes - and
`CompiledStyleSheet::matched_rules()` is the same list `add_to_cascade()` consumes, so a rule the
inspector names is a rule the cascade used.

**`overridden` is computed, not guessed.** A declaration is marked when a higher-precedence
declaration won its property, decided by the cascade's own `cascades_over()` over the same
`(origin, importance, layer, specificity, source_order)` keys the cascade used. Source order alone
would be wrong the first time two rules of different specificity set one property.

**The provenance the slice exists for.** `CompiledRule` carries `selector` (the authored text) and
`offset` (the rule's byte offset in its sheet), threaded from `PreparedItem` at prepare time. Before
this slice the cascade had the parsed `SelectorList` and the `CompiledRule` in hand and dropped both,
so nothing could name the rule a developer would recognise. `CompiledRule::source_path` names the
sheet the offset is an offset into, stamped by the ordered-combination constructor from the
`StylesheetSource` each rule came from.

**The blob does not carry them, and that is deliberate.** `docs/specs/compiled-stylesheet-v1.md`
section 5 defines the versioned blob; adding the two fields would bump `format_version` and
regenerate every committed blob golden for a field only a design-time query reads. A blob-loaded
sheet therefore has no authored text and no source file, so the resolve step prints the selector back
from the parsed list (`src/ui/style/selector_text.cpp`, not installed) and reports offset 0. The
inspector's answer is the same either way for the selector, which is what a developer reads; the
span is empty because a build artifact has no source file to point into. The decision is asserted in
`tests/unit/stylesheet_blob_tests.cpp` (`provenance_survives_a_blob_as_text_only`).

**A miss is a real answer.** `ok:false` with `reason:"no_such_node"` and **no other field**: a page
that read a stale `computed` off a failed answer would show the previous node's style under the new
node's name. An id of 0 is the engine's "no automation id", so it can never name a node.

**The page half.** `designer/web/src/views/canvas-inspector.ts` owns the panel: a page element beside
the canvas, `hidden` until a node is inspected, filled with the computed table, one block per matched
rule (an overridden declaration is struck through rather than dropped - the point of the panel is to
answer "why is my rule not applying") and the box. It is **off by default** and armed by the
`Inspector` toggle in the Canvas pane's header (`aria-pressed` carries the state), because an armed
inspector consumes the click it inspects: the listener is registered in the capture phase and stops
the event when the inspector took it, so the engine's pointer path never sees a press that was meant
to select a node. A visitor who never opens it keeps a fully interactive canvas. The panel is a page
element rather than engine paint for the same two reasons the picker's overlay is: an engine-drawn
panel would be part of the frame and would contaminate the pixels a golden compares, and ADR-054
decision 3 says the engine draws the previewed app and nothing else.

### 5.3 The commit's patch (issue #171)

The third and last slice of #171 is `source.patch`, the write half of
[`designer-protocol-v1.md`](../specs/designer-protocol-v1.md) section 10.2. Given a node and a
destination, the viewer produces the **minimal list of `TextEdit`s** a host applies, plus the
**base versions** the host compares. It writes nothing: applying the edits and answering
`source.patch.result` is the studio's project-IO path (section 10.3), and this slice does not build
it.

**The engine gap this slice closed.** The IR records the `SourceSpan` of the JSX element each node
came from (`ir::Node::source`), but the scene node did not keep it: `scene_build_walk.cpp` created
the `ui::Node` and dropped the span, so nothing downstream could name the bytes a commit would edit.
The span now travels in a **side table** rather than as a `ui::Node` field. `ui::Node` is a public
header with an enforced `kNodeSizeBudget` and a `static_assert`, and a span is a design-time fact no
frame reads; growing every node for it would be a memory-budget decision taken for a query. The table
is `SceneBuildResult::SceneNodeSource { node_id, span }`, filled by the build walk (the last code
that holds the `ir::Node`), owned by `PreviewSession` and replaced by every rebuild, so a span always
describes the tree on screen. `CanvasSceneSession::node_span()` is the read.

**Why the attribute range comes from the parser.** The IR keeps the element's span, not its
attributes', so the `inline` destination re-parses the component's file and finds the element whose
span starts where the node's does - the two offsets are the same number, because the lowering copies
the AST span verbatim. `JsxAttribute::span` is then the exact byte range of the `style` attribute. A
`find("style=")` over the text would match a string literal, a comment or a nested element, and the
first time it did, a commit would rewrite the wrong bytes. The `class` destination uses
`style::parse_stylesheet` for the same reason: `Rule::offset` and `Declaration::offset` are the
parser's own measurements, so a rule the commit edits is a rule the cascade would have read.

**The three destinations.**

| destination | edit | when the target is absent |
| --- | --- | --- |
| `inline` | replaces the `style` attribute's bytes with the merged attribute text | inserts one (`length == 0`) at the end of the opening tag |
| `class` | replaces the declarations the commit names, inserts the ones it adds | appends the whole rule to the file |
| `token` | replaces the value of the existing token declaration in the active theme's object | rejects: the spec edits the *existing* declaration |

`inline` keeps every declaration the attribute already had, byte for byte, and only replaces the
properties the commit names: dropping or reformatting the others would silently rewrite a value
nobody asked to change. `class` never creates a CSS file - the file must already be a project file,
which is the spec's rule and also the only way the base version can exist. `token` edits the theme
asset (`assets.theme`, `stylesheet-v1.md` section 9.1), which is JSON, not CSS: the value is located
by scanning for the string literal of `"<token>"` inside the active theme's object, so key order,
indentation and every other token survive untouched. A token the active theme does not define is a
rejection, not an append.

**The ordering rule, and what it actually guarantees.** `edits` is sorted by file and **descending**
offset, and the ranges do not overlap. The property that buys is not "any application order gives the
same text" - that is false whenever two replacements change the file's length, because applying
ascending shifts the second edit. It is that **applying the list in the given order equals splicing
every range against the original offsets at once**, which is what a host's applier must satisfy.
`scene_canvas_patch_tests.cpp` asserts exactly that, against a simultaneous-splice reference, and
asserts descending order and non-overlap directly.

**UTF-8.** `offset` and `length` are byte offsets and must land on code point boundaries (section
3.3), so every range is checked with `is_code_point_boundary()` before it becomes an edit; a range
that would split a character is `component_invalid` rather than a corrupted file. A test with a
two-byte `é` before the edit point asserts the range is still exact.

**The failure vocabulary is the spec's, exactly**: `no_overrides`, `not_token_bound`,
`no_such_node`, `no_such_class_file`, `component_invalid`, `stale_source`. No patch is produced for
any of them, and the report carries no `edits` and no `baseVersions` - a page that read an empty edit
list off a failure would show "0 edits" where the truth is that nothing was produced.

**What is narrower than section 10.1, and why.** A commit writes *all of a node's overrides*
together, and the override layer is `style.override.set` - a later slice. This slice therefore has no
overrides to commit, so the page sends an empty declaration list and the engine answers
`no_overrides`. The `token` destination's binding test is against the node's inline `style` value
being `var(<token>)`, because that is the only override-shaped value that exists today. Both are
narrower than the spec and both are reported rather than papered over: the engine does not invent an
override layer to make a commit succeed.

**The page half.** `designer/web/src/views/canvas-inspector.ts` gained a `commit` section: one button
per destination, and the answer the last commit produced. Since #585 the studio page IS the host: the
commit round trip (`lib/override-client.ts`) applies the returned `source.patch` to the file buffers and
settles the overrides, so the section reports `committed`, a `patch rejected (version_mismatch)` that kept
the overrides, or the spec's reason, instead of the earlier "produced — not applied (no host)" edit list.
The override rows, the colour control, the refused-value diagnostic and Undo/Redo routing are described
in [designer-protocol-v1.md section 9.6](../specs/designer-protocol-v1.md#96-page-behavior-issue-585).

### 5.4 The viewport limits (issue #690)

The canvas is sized from numbers a person types, a device profile provides, or a raw `viewport.set`
carries. Before this, `canvas_viewport_request` refused only non-positive values, so a large size at a
high DPR could ask for a backing store beyond what a render target can be - and the failure is not a
clamped preview but a lost device or a failed surface creation, which is the case the issue calls out.

One rule, in `tools/codegen/canvas_viewport_request.cpp`, applied on **both** sides of the ABI:

| Limit | Value | Why |
|---|---|---|
| Extent | 1..8192 | Every WebGPU implementation reports a canvas maximum of at least 8192; the page's limit, deliberately not the device's own `maxTextureDimension2D` (the module has no device when it parses a viewport, and asking one would make legality hardware-dependent) |
| DPR | 0.01..4 | A scale, not a count; `0.01` is the smallest a zoom can ask for and `4` the largest a real device on this tree reports |
| Backing store | 256 MiB | extent x DPR, 4 bytes a pixel. Keeps a 4096x4096 rectangle at DPR 4 (1 GiB unclamped) inside one texture's worth of memory |

Two behaviours, and they are different on purpose:

- **Over a limit: clamped, and reported.** `clamped: true` in the report carries the size the engine
  applied, and the pane names it ("viewport clamped to 8192x8192 at DPR 2"). A clamp never changes the
  *logical* extent for the byte cap - it reduces the DPR - so a clamp cannot move what `@media` sees.
- **Not a size at all: refused.** A zero, negative, NaN or infinite extent or DPR answers `error` and
  leaves the picture that is already there, because "the largest legal size" is an answer to a
  question the caller did not ask.

The page holds the same rule in `designer/web/src/views/canvas-viewport.ts` (`clampViewport`,
`requestedViewport`, `sizeToDevicePixels`), because it sizes the backing store itself: a size only the
module clamped would still reach the browser unclamped. The two copies are pinned against each other -
`tests/unit/canvas_viewport_limit_tests.cpp` for the engine and `scripts/ci/preview_viewport_limits.mjs`
for the page assert the same table, so a change to one side fails the other.

### 5.5 The device scale (issue #599)

The tree is laid out in CSS pixels; the canvas target is the device-pixel grid. Those are two different
spaces, and the app's frame composes them every frame (`src/app/window_frame.cpp` sets
`set_content_scale`, `PaintContext::device_scale` and the vector sink's scale together). The preview
composed nothing, so a DPR-2 canvas laid the scene out at 320x240 and then painted it 1:1 into the
top-left quarter of a 640x480 surface - the reported symptom was text at half size with the rest of the
canvas left clear.

`scene_frame_paint_tree()` now takes the scale: `options.dpi_scale` from the single-frame path,
`CanvasSceneSession::dpi_scale()` from the canvas session (the value the running evaluation was laid out
at, so the two cannot disagree). At 1.0 the renderer's `begin()` already leaves the transform at
identity and the call is skipped, which is why every existing golden is byte-identical.

`tessera_canvas_draw` also receives the **physical** extent now, not the logical rectangle: the target
is the texture the page configured, and the two agree only when no clamp reduced the DPR (see §5.4).

The parity corpus's `dpr` case is the regression: a full-bleed box at DPR 2 must show its colour at the
far corner of the doubled target. A check for "the clear colour is absent" would pass on a frame that
painted nothing at all, so the assertion is on the box's colour.

**Why the page half is only geometry.** A presented WebGPU frame is not composited by headless
Chromium, so `drawImage` and `screenshot()` both return zeros however correct the paint is (the same
limitation `canvas-session.ts` records for its own screenshot path). `scripts/ci/preview_browser_canvas_size.mjs`
therefore asserts what the page owns - the backing store, the CSS box, and their agreement with the
reported DPR, at DPR 1 and 2 - and leaves the pixels to the lavapipe case above.

### 5.6 The drag-resize handles (issue #690 slice b)

The canvas' size has three authors: the toolbar's menus, a raw `viewport.set`, and now three grips on
the canvas' own edges (right, bottom, corner). A drag is the only one that can produce a hundred
sizes a second, so it is the only one with a rate rule.

**One step per display refresh.** The grip records the pointer's travel per move, paints the size it
would apply into a page-only readout, and asks for one coalesced step; the step is the unit that
reaches the engine. Pointer-up runs exactly one settled step, so the size the person released on is
the size the engine ends with even when the release falls between two animation frames. The rule is
`shouldStep` in `canvas-resize.ts` and it is gated in Node (`preview_canvas_resize.mjs`), because a
*browser* cannot show it: the session's own `requestFrame()` already coalesces frames, so a per-move
`apply` measures identically in the page. The page gate checks the property a person can see instead -
a burst of 50 moves costs at most a frame per paint, and the settled size is exact.

**The divisor is the zoom, not the DPR.** The canvas' CSS box is the *logical* viewport
(`sizeToDevicePixels` sets `style.width` from the logical width), so one CSS pixel of travel is one
logical pixel however many device pixels it covers. Dividing by the DPR halved every drag at DPR 2.
The zoom, by contrast, must be divided out - and it is read from the stage the pane transforms, not
from the canvas, because a CSS transform scales an element's rect and its layout box alike.

**Wiring is delegated, not bound.** The session is created from refs that exist at the first settle,
which is before React commits the grips and the readout, so a listener looked up by id finds nothing
and the gesture silently does nothing. The layer listens on the document in the capture phase and
resolves the readout on first write; capture is also what makes the grip win the press before the
engine's pointer path sees it, so a grip never moves `:hover` and never activates the control under it.

**The result is a toolbar value.** A drag calls the page's own `setCustomSize`, so the profile chip
reads `custom` afterwards, the size is remembered like any other control change, and skins, zoom and
the DPR stay consistent with it. The reached size goes through `clampViewport`, so a drag cannot reach
a size a menu could not, and a clamped one says so on the readout.

### 5.7 Animation (issue #603)

The canvas used to run no animation runtime and ask for no second frame, so `:hover` changed style
instantly, `@keyframes` never played and a transition could not be seen. The session now owns a
`CanvasMotion` driver (`scene_canvas_motion.{hpp,cpp}`): the production `AnimationRuntime` and
`FramePipeline` (the pair `AppHost` runs) over a `RuntimeLoop` whose clock is a `ManualClock`.

- **The clock is the page's.** `tessera_canvas_tick(now_ms, reduced_motion)` takes the `requestAnimationFrame`
  timestamp; the driver feeds the difference to the `ManualClock`, ticks the pipeline once (`dt` clamped to
  250 ms, so a hidden tab does not fast-forward an animation) and syncs style against the live tree, as
  `AppHost::begin_frame(dt, alpha)` does. The same timestamps always give the same values, which is what
  `scene_canvas_motion_tests` relies on. The page calls tick, then draw, in one refresh.
- **What reaches the paint pass.** The animated values (opacity, colors, transforms: the paint-only samples
  the native paint view carries) go to `scene_frame_paint_tree` as a `PaintView`; an idle scene hands it
  none and paints exactly what it did before. Layout-affecting properties stay discrete, as natively.
- **Keep-alive, idle is zero frames.** `animating()` is true while the runtime holds an entry or a spring;
  `needs_frame()` includes it, `tessera_canvas_draw`'s report carries it, and the page
  (`canvas-motion.ts`) requests the next refresh while it is true and nothing once it is false. The
  first idle timestamp re-anchors the clock, so the gap between two interactions is not simulated time.
- **Reduced motion.** The page passes `matchMedia("(prefers-reduced-motion: reduce)")` with each tick; the
  runtime's clamp applies to animations that start while it is set (it does not rewrite running ones).
- **State survives.** The driver belongs to an evaluation: a click (a re-evaluation of the tree), a
  `set_viewport` or a posture change keeps it, so an animation keeps its progress and hover keeps its
  baseline; a new `start()` that evaluates replaces it, and its first frame records the style baseline
  (without which the first hover would be a first sighting and start no transition).
- **The style sync runs only for a sheet that declares motion** (`@keyframes`, `transition*`, `animation*`),
  so a static scene's pixels are what they were before the driver existed. The sync also applies
  `z-index` and `transform` to the scene, which the font-free native frame CLI does not; a sheet with
  `animation` and `transform` therefore differs from `tessera_scene_frame` by design.

Not done here: `useAnimatedValue(...).set(..., {animate})` (`scene_build.cpp` evaluates the cell to its
initializer), springs, and the `prefers-reduced-motion` media query's answer in `@media`. Gates:
`scene_canvas_motion_tests` (engine, manual clock) and `scripts/ci/preview_canvas_motion.mjs` (page loop).

## 6. The page half (landed)
The page half landed after the React conversion (#422) as `designer/web/src/views/canvas-pointer.ts`,
attached by `canvas-session.ts`; `scripts/ci/preview_browser_canvas_pointer.mjs` is its browser acceptance
(hover, press, click, leave, a drag-off, and the frame counter staying put for a static scene). Two details
differ from the sketch below. The element's drawable box is inside its border, so the conversion uses
`clientLeft/Top/Width/Height`, not the bounding rect. And a click whose handler ran **rebuilds the tree**,
which resets the router, so the page sends one more `move` at the same position after such an `up`
(`ran` non-empty): that is the requirement `scene_canvas_pointer_tests.cpp` writes down, and without it a
button would lose its hover look after every click. The original sketch:

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

**The style inspector slice (issue #171, 2026-11-19).** Added `tessera_canvas_style` and its report,
the `scene_canvas_style` ctest case, the provenance threading (`PreparedItem` -> `CompiledRule`), the
selector printer, and the page's panel. Verified on this Windows host:

* `cmake --build build\vs2026 --config Release --parallel 12 --target scene_canvas_style_tests`:
  exit 0, `/WX` clean.
* `scene_canvas_style: ok`. The case asserts the reported `background-color` against the value the
  cascade resolves for the same node (not a constant), that `#target` (one id) is listed before
  `.card` (one class) although the source order is the opposite, that `.card`'s `background-color` is
  `overridden` while its `color` is not, that the box equals the node's own `world_bounds()` field by
  field, and that a missing id and id 0 both answer `found:false` with no computed style and no box.
* `scene_canvas_pick: ok`, `scene_canvas_pointer: ok`, `scene_canvas_session: ok`,
  `compiled_stylesheet: ok`, `stylesheet_blob_tests` exit 0 - the provenance threading did not move
  the cascade, and the new blob case asserts that a blob-loaded rule reconstructs the same selector
  text and reports offset 0.
* `cmake --build build/wasm-webgpu --target tessera_canvas_wasm`: exit 0, and the linked
  `tessera_canvas.js` contains `Module["_tessera_canvas_style"]` in its export table. A *build* fact,
  not a runtime one.
* `npm run build --prefix designer/web` (typecheck + site + SSR chrome): exit 0.
  `node scripts/ci/preview_page_checks.mjs`: all pass, including the canvas pane's 23 ids (the
  inspector's toggle, panel and body are three of them).
* `python -B scripts/check_preview_budget.py --check --size-only`: 0 errors. The breakdown was
  re-recorded (`--size-only`, so no local timing reached a CI pin) and the 10 MB owner-decision
  ceiling was restored by hand, as the file's note records.

**Not verified: the panel in a browser.** The inspector's click path, its capture-phase
`stopPropagation` and the panel's layout have not been exercised against a real WebGPU device or a
real pointer, for the same reason the picker's overlay has not: the browser probe needs a
Playwright-class dependency this lane does not have. The engine half is proven on the null RHI; the
page half is proven to build, typecheck and render its ids.

## 8. Files

| Path | Role |
|---|---|
| `tools/codegen/scene_canvas_session.{hpp,cpp}` | the persistent session: evaluation, pointer dispatch, frame accounting, and the picker's `pick()` query |
| `tools/codegen/scene_canvas_inspect.{hpp,cpp}` | the style inspector's query: computed style, matched rules with their provenance, and the box, for one node |
| `tools/codegen/scene_canvas_motion.{hpp,cpp}`, `wasm_canvas_motion.cpp` | the animation driver, its tick report and `tessera_canvas_tick` (issue #603) |
| `designer/web/src/views/canvas-motion.ts` | the page's animation frame loop (keep-alive, no double chain) and the reduced-motion read |
| `tools/codegen/scene_frame.{hpp,cpp}` | `scene_frame_paint_tree()` - paint a caller-owned tree; `scene_frame_paint()` is now its wrapper |
| `tools/codegen/canvas_report.{hpp,cpp}` | `canvas_pointer_report_json()`, `canvas_pick_report_json()` and `canvas_style_report_json()` beside the existing frame report (host-compilable) |
| `tools/codegen/wasm_canvas_entry.cpp` | the C ABI: `tessera_canvas_apply/pointer/pick/style/draw/reset`, with the coordinate rule on the exports |
| `tools/codegen/scene_prepare.cpp` | `preview_stylesheet()` gains `Button:hover`, `Button:active`, `TextInput:hover`, `TextArea:hover` |
| `tools/codegen/scene_session.{hpp,cpp}` | `last_ran()`/`last_unrun()`/`router()` accessors, so the session can report what a dispatch did |
| `tests/unit/scene_canvas_session_tests.cpp` | lifecycle + the frame contract (a static scene draws zero) |
| `tests/unit/scene_canvas_pointer_tests.cpp` | the pointer path into the style engine, click synthesis, ignored buttons, cancel |
| `tests/unit/scene_canvas_pick_tests.cpp` | the picker query: the right node and box, a miss, and that a pick changes nothing |
| `tests/unit/scene_canvas_style_tests.cpp` | the inspector query: the cascade's computed values, precedence order, `overridden`, the box, `no_such_node`, and the report document |
| `tests/unit/scene_canvas_test_support.hpp` | the shared sources, tree helpers and the on-demand `CanvasHarness` |
| `src/ui/style/selector_text.{hpp,cpp}` | the selector list printed back from its parsed form, for a sheet that arrived as a blob |
| `designer/web/src/views/canvas-picker.ts` | the page's overlay: the outline, the readout, the toggle and the CSS-to-logical conversion |
| `designer/web/src/views/canvas-inspector.ts` | the page's inspector panel: the computed table, the matched rules, the box and the toggle |
| `designer/web/src/regions/CanvasPane.tsx` | the overlay element (`pointer-events: none`), the inspector panel, and the two toggles |
| `src/compiler/codegen/CMakeLists.txt` | the module-local build wiring (see the answer accompanying this landing) |
