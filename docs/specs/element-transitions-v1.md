# Element transitions v1: AnimatePresence and SharedElement

Status: proposed (component library stage 2, [`component-library.md`](../design/component-library.md)
section 8; ships after V1). It extends [`view-transitions-v1.md`](view-transitions-v1.md) (window-wide
transitions) with per-element behavior, and so supersedes the "no per-element `view-transition-name`"
limit of that spec's section 1 and of [ADR-035](../../decisions/ADR-035-view-transitions.md)
alternatives. A decision record (ADR) is written once the open questions of section 12 are answered.
Builds on [`animation-v1.md`](animation-v1.md), [`ir-v1.md`](ir-v1.md),
[`component-model-v1.md`](component-model-v1.md), [`navigation-v1.md`](navigation-v1.md),
[`accessibility-v1.md`](accessibility-v1.md) and [`automation-ids-v1.md`](automation-ids-v1.md).

Terms: **Presence** is the enter/exit lifecycle of a node that appears or disappears from the tree.
**Exiting node** is a node whose condition or list item went away but which stays in the scene until
its exit animation ends. **Shared element** is one logical element that exists in the old and the
new UI under the same name and animates from its old to its new rectangle. **Name** is the string
that pairs the two. **Group** is the per-name box that moves and sizes during the transition.
**Snapshot** is a texture capture of one node's subtree. **FLIP** is "first, last, invert, play",
the technique of animating a layout change by transforming from the old position. See also the
[glossary](../reference/glossary.md).

## 1. Goals and non-goals

Goals:

- Animate a node entering and leaving the tree (`AnimatePresence`) and an element moving between
  two screens or states (`SharedElement`), without a JavaScript runtime and with deterministic
  output.
- Reuse the animation runtime, the keyframes engine, the view-transition compositor and the
  pseudo-element CSS model; add the smallest new surface.
- Keep the rule of ADR-035: **state commits first**. The new UI owns input, focus, semantics and
  automation immediately; animation is only paint.

Non-goals (not in this spec): layout animation of siblings (FLIP reflow, open question 1), gesture-driven
transitions (interactive back swipe), transitions limited to a subtree, `wait` mode, and a
physics-based shared-element path. Each needs its own design.

## 2. Rules that apply to both features

1. **No behavior depends on animation.** Removing every animation (reduced motion, low power,
   budget skip) leaves the same final tree, focus, semantics and automation ids.
2. **Bounded.** Exiting nodes, snapshots and bytes have limits (section 9); exceeding one degrades
   to an instant change and a diagnostic, never an error.
3. **Deterministic.** Progress comes from the simulation clock; names and order are never random.
4. **Idle afterwards.** When the last exit or shared group finishes, nothing is scheduled and no
   present follows ([ADR-019](../../decisions/ADR-019-power-aware-runtime.md)).
5. **Developer C++ is not involved.** Both features lower from the TSX; developer classes only
   see the final tree.

## 3. AnimatePresence

### 3.1 Authoring

```tsx
<AnimatePresence initial={false} layout="pop">
  {open && <Card key="detail" className="card" />}
  {items.map((item) => <Row key={item.id} item={item} className="row" />)}
</AnimatePresence>
```

```css
.card:entering { animation: card-in var(--duration-normal) ease-out both; }
.card:exiting  { animation: card-out var(--duration-fast) ease-in both; }
```

`AnimatePresence` is a library component the compiler recognizes. It draws no node and has no
box (the "ignored wrapper" rule of [`accessibility-v1.md`](accessibility-v1.md)); it marks each
**direct** child that carries a static condition or a keyed loop as presence-controlled. Its props:

| Prop | Type | Default | Notes |
|---|---|---|---|
| `initial` | bool | `false` | `false`: nodes already present when the presence first mounts do not run `:entering` |
| `layout` | `keep`, `pop` | `keep` | What an exiting node does to layout (section 3.4) |

A direct child that has neither a condition nor a loop is never removed by this component and gets
the diagnostic `TESSERA_PRESENCE_CHILD` (warning). A loop child needs `key` (already a compile
error without one, [`component-model-v1.md`](component-model-v1.md)).

### 3.2 IR and lowering

The IR gains an additive optional node field `presence: {layout, initial}` on a node that has
`condition` or `loop` ([`ir-v1.md`](ir-v1.md) section 3). It is additive, so `irVersion` stays `1`.
For such a node codegen emits a small state machine instead of the destroy-on-false step:

```text
Absent --cond true--> Entering --animations done--> Present
Present --cond false--> Exiting --animations done or cap--> Removed(Absent)
Exiting --cond true--> Present      (reverse; section 3.5)
Entering --cond false--> Exiting    (retarget from the current visual value)
```

Without `presence`, lowering is unchanged: a false condition destroys the node at once.

### 3.3 Styling states

`:entering` and `:exiting` are Tessera pseudo-classes (an extension of the pseudo-class list in
[`stylesheet-v1.md`](stylesheet-v1.md) section 6). `:entering` matches from the first style pass of
a new node until its `animation`s end (or at once if it has none); `:exiting` matches from the
commit that removed it until removal. They match the presence-controlled node and its
descendants, so one rule on the root styles the whole subtree.

The animation runtime starts the animations through the normal path (idempotent ensure, retained
fills, reduced-motion clamping, [`animation-v1.md`](animation-v1.md)). There are no new animation
primitives. The framework default styles define three opt-in keyframes the developer can name:
`tsr-fade`, `tsr-scale` and `tsr-slide-up` (each with `-in` and `-out` forms, opacity and
`transform` only).

### 3.4 What an exiting node is

| Aspect | Rule |
|---|---|
| Layout | `layout="keep"`: it keeps its box until removal, then siblings reflow at once (a visible step; see open question 1). `layout="pop"`: it leaves the flow at the commit and is drawn at its last bounds (absolutely positioned), so siblings reflow at once and nothing overlaps unexpectedly |
| Input | `pointer-events: none` for the node and descendants; it is not in the hit test |
| Focus | If focus was inside, it moves at the commit to the logical next target, not at the end of the animation ([`animation-v1.md`](animation-v1.md) section 8) |
| Semantics | Removed from the accessibility tree at the commit |
| Automation | Stays in the headless snapshot with `presence: "exiting"` and its original ids, non-actionable, so a test can wait for it to disappear |
| Bindings | Frozen at the last committed values: no further updates, no effects, no handlers |
| Paint | Normal, in its original paint order, until removal |

### 3.5 Reversal, interruption and lists

- If the condition turns true again while the node is `Exiting`, the **same node** returns to
  `Present`: no new instance, same ids, animations retarget from the current visual value (the
  rule of [`animation-v1.md`](animation-v1.md) section 8). State owned by the subtree is intact
  because it was frozen, not destroyed.
- A keyed list item removed and re-added with the same key behaves the same way; a re-add after
  the exit finished is a new instance with the same `(uiId, itemKey)`.
- Reordering a keyed list triggers neither enter nor exit.
- A node that appears while its parent is itself exiting is created in `Present` with no
  `:entering` (the parent's exit already hides it).

### 3.6 Completion and caps

A node is removed at a tick boundary once every animation on it and its descendants has finished.
Two caps keep this bounded:

- **Time:** `presenceExitTimeoutMs` (default `1000`, [`runtime-config-v1.md`](runtime-config-v1.md)).
  An animation still running at the cap (for example `animation-iteration-count: infinite`) ends
  the exit and gives `TESSERA_PRESENCE_TIMEOUT`.
- **Count:** `presenceMaxExiting` (default `32`) exiting nodes per presence. Past it the oldest
  exiting node is removed at once.

## 4. SharedElement

### 4.1 Authoring

```tsx
<Image viewTransitionName={"photo-" + item.id} src={item.src} />
<SharedElement name={"title-" + item.id}><Text>{item.title}</Text></SharedElement>
```

- `viewTransitionName` is an attribute of the intrinsic elements that lowers to the CSS property
  `view-transition-name` (inline style). The same property can be set from a stylesheet.
- `SharedElement` is library sugar: a `View` that sets `view-transition-name` from `name` and adds
  the part `shared`.
- The value is a string; a loop child builds it from its key. Only elements with a **non-`none`**
  name take part; `root` stays reserved for the window image.

A name is unique per UI snapshot. Two nodes with the same name in one snapshot give
`TESSERA_VT_DUPLICATE_NAME` and neither takes part (both draw as ordinary content).

### 4.2 When a shared transition runs

Names are paired when a transition runs; they never animate on their own. Three triggers:

1. Inside `startViewTransition(update, options)`
   ([`view-transitions-v1.md`](view-transitions-v1.md) section 3.1): named elements of the old and
   new UI take part automatically, with the root effect running underneath.
2. A `Navigator` push, pop or replace ([`navigation-v1.md`](navigation-v1.md) section 7): when the
   outgoing or incoming screen contains a named element, the Navigator runs the route change as
   a view transition whose root effect is the route's transition (or `none`).
3. A state change in plain code does **not** pair names; wrap it in `startViewTransition`.

### 4.3 Capture and pairing

Extending the lifecycle of [`view-transitions-v1.md`](view-transitions-v1.md) section 4:

1. **Capturing.** The old root image is rendered with every named subtree omitted (so the
   element does not appear twice). It is always re-rendered, never reused from the last frame.
   Each named subtree is rendered into its own old snapshot, clipped to its border box, at
   display size times device pixel ratio. Its window-space rectangle (including ancestor
   transforms and scroll offsets) is recorded.
2. **Updating and layout** as before.
3. **Pairing.** The new UI's named nodes are collected the same way: new root image without them,
   a new snapshot and a rectangle each.
4. **Composing.** For each name:

| Case | Visual |
|---|---|
| In old and new | One group moves and sizes from the old to the new rectangle; the old snapshot fades out and the new fades in inside it |
| Only in old | The old snapshot stays at its rectangle and fades out |
| Only in new | The new snapshot stays at its rectangle and fades in |

The real nodes of the new UI are not drawn while their group runs (their snapshot stands in), and
they are drawn normally again after `Finished`. A named node that is clipped, hidden or off-screen
in one snapshot is treated as absent from that side.

### 4.4 Pseudo-elements and CSS

Section 5 of [`view-transitions-v1.md`](view-transitions-v1.md) allows only `root`; this spec
allows any name:

```text
::view-transition
 ├─ ::view-transition-group(root) ... (unchanged)
 └─ ::view-transition-group(<name>)          (one per paired or unpaired name, above root)
     └─ ::view-transition-image-pair(<name>)
         ├─ ::view-transition-old(<name>)
         └─ ::view-transition-new(<name>)
```

Groups are ordered above the root group in the paint order of the new UI (ties by name, ascending,
so the order is deterministic). Properties allowed on these pseudo-elements are those of that
section, plus `width`, `height` and `transform` on `::view-transition-group(<name>)` only (a
paint-time box; it never lays out other nodes). Per-group custom properties, set on the group:

| Property | Value |
|---|---|
| `--vt-from-x`, `--vt-from-y`, `--vt-from-w`, `--vt-from-h` | Old rectangle in px (absent for only-new) |
| `--vt-to-x`, `--vt-to-y`, `--vt-to-w`, `--vt-to-h` | New rectangle in px (absent for only-old) |

The framework default styles animate the group with the built-in `shared-move` keyframes
(translate and size from the old to the new rectangle, easing and duration inherited from the
transition) and cross-fade `old` and `new`. A developer overrides them with the same selectors and
`@keyframes`, as for `root`. The images are drawn stretched to the group box (`fill`); text is
scaled, not re-rendered, during the move (open question 3).

### 4.5 Input and focus

The new UI owns input at its final rectangle from the first composed frame, even while the
snapshot is still moving: a tap on the final position hits the real node. Focus and the
accessibility tree describe the new UI immediately, so a focused shared element keeps focus.

## 5. Accessibility

- Neither feature changes the semantics tree beyond the rules above: new nodes appear at the
  commit, exiting nodes leave at the commit, snapshots and groups are not nodes.
- A route or content change that animates is still announced by the control or route that caused
  it, once, never by the animation.
- Reduced motion: section 6.

## 6. Reduced motion, power, low power

| Condition | `AnimatePresence` | `SharedElement` |
|---|---|---|
| Reduced motion | `:entering` and `:exiting` animations are clamped by the animation runtime (instant or opacity only, at most 120 ms); nodes are removed as soon as they end | Groups do not move: each name cross-fades in place at the new rectangle for at most 120 ms |
| Low power | Unchanged (animations are cheap) | Names are ignored; the root effect runs as `crossfade` |
| Battery saver | Animations are skipped; nodes are removed at once | The whole transition is skipped (rule of view-transitions section 9) |

While anything animates the pacer wakes at the animation cadence; when it settles nothing is
scheduled (render-on-demand).

## 7. Interaction with view transitions

- **A presence change made inside a `startViewTransition` update function** does not start
  `:entering`/`:exiting` animations for that recalculation (the rule that stops the same change
  being animated twice, [`view-transitions-v1.md`](view-transitions-v1.md) section 4.2): the node
  appears or disappears at once and the view transition draws the change.
- A presence change made **while** a view transition is composing runs normally inside the new
  image.
- A resize, DPI change, hide, occlusion or fold change that skips the view transition also ends
  every shared group at once; exiting nodes are removed at once.
- A second `startViewTransition` while one composes follows the interruption rule of that spec:
  groups of the running transition end, and the next one captures the current composed output.

## 8. Diagnostics

Records follow [ADR-022](../../decisions/ADR-022-diagnostics-and-profiling.md): `TESSERA_PRESENCE_CHILD`
(a direct child never leaves), `TESSERA_PRESENCE_TIMEOUT`, `TESSERA_VT_DUPLICATE_NAME`,
`TESSERA_VT_NAME_BUDGET` (names beyond the budget), and for each transition the paired, only-old
and only-new counts and the snapshot bytes. Pixel contents are never logged.

## 9. Resources

- Snapshots come from the framework texture pool
  ([`resource-lifecycle-v1.md`](resource-lifecycle-v1.md)) and are released by its retirement rules.
- Shared elements are limited by `viewTransitionMaxSharedElements` (default `8`) and share the
  `viewTransitionMaxMb` budget of [`view-transitions-v1.md`](view-transitions-v1.md) section 9. Names
  are admitted in paint order until either limit would be exceeded; the rest are treated as
  unnamed (`TESSERA_VT_NAME_BUDGET`) and the transition still runs. Only when even the root pair
  does not fit is the whole transition skipped.
- No allocation per frame; snapshots are made at Capturing and Composing start only.
- Exiting nodes hold their scene nodes and bindings but no GPU resources of their own.

## 10. Configuration keys (added to `runtime-config-v1`)

| Key | Default | Range | Meaning |
|---|---|---|---|
| `presenceExitTimeoutMs` | `1000` | `0..10000` | Longest exit before forced removal; `0` removes every exiting node at once |
| `presenceMaxExiting` | `32` | `1..1024` | Exiting nodes kept per presence |
| `viewTransitionMaxSharedElements` | `8` | `0..64` | Shared elements per transition; `0` disables sharing |

## 11. Testing

All tests use a fixed step, fixed start time and the manual clock; no wall clock.

- Unit, presence: state machine transitions including reverse and retarget; keyed list add,
  remove, reorder; frozen bindings; both caps; `initial`; `layout` keep and pop; no-animation
  node removed at once; presence change inside a view transition starts no animation.
- Unit, shared: capture rectangles with ancestor transform and scroll offset; pairing (both,
  only-old, only-new); duplicate names; budget admission order; virtual-list items outside the
  realized window; reduced-motion cross-fade; resize skip.
- Headless ([`headless-automation-v1.md`](headless-automation-v1.md)): exiting node listed with
  `presence: "exiting"` and non-actionable; focus moved at commit; a tap at the final rectangle
  during a shared move hits the real node; automation ids unchanged.
- Golden (lavapipe, [`golden-tests-v1.md`](golden-tests-v1.md)): enter and exit at progress 0, 0.5,
  1; a shared move at progress 0, 0.25, 0.5, 0.75, 1 for a card-to-detail case; right-to-left;
  DPI 1 and 2; default and dark themes.
- Resources: no leaked textures after skip, interruption or resize; zero presents after the last
  group ends.
- Compiler: `presence` lowering matches a stored golden; a non-conditional direct child warns.

## 12. Open questions

1. **Sibling reflow animation.** `layout="keep"` and `pop` both reflow siblings with a step. A real
   fix is layout animation (FLIP or animating layout properties), deferred in
   [`animation-v1.md`](animation-v1.md) section 10.
2. **`wait` mode** (new child appears only after the old one has left): it conflicts with "state
   commits first" and is left out; confirm it stays out.
3. **Shared images.** Stretching a snapshot distorts content whose aspect ratio differs between
   the two rectangles. Options: always `fill`, a per-name `cover` or `contain`, or re-render the
   new subtree at the moving size.
4. **Named elements and ancestor clips.** Groups ignore ancestor clips while moving. Confirm this
   (it matches the web) or clip to the common ancestor.
5. **Name expressions in styles.** `viewTransitionName` takes an expression only as an attribute;
   a stylesheet value is a literal. Confirm.
6. **Navigator default.** Whether a Navigator route change with named elements always starts a
   view transition (this spec) or only when the route opts in.
