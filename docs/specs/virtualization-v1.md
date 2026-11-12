# UI virtualization v1: virtualized lists and grids

Status: proposed (Big Steps B2 and E, defined in [`AGENTS.md`](../../AGENTS.md#big-steps-group-work-dont-micro-step): B2 = runtime slice, E =
data-heavy UI). This is **UI/data virtualization**, not VM/container
virtualization and not a virtual DOM. Scroll physics/nested scroll:
[`scroll-and-gestures-v1.md`](scroll-and-gestures-v1.md).

Terms: **V1** is the first release scope ([`AGENTS.md`](../../AGENTS.md#non-negotiables), item 8).
**Realized** means a node exists in the scene graph for the item. **Overscan** is the extra range
realized beyond the viewport. **Scene graph** is the retained node tree
([`scene-graph.md`](../design/scene-graph.md)). **IR** is the compiler intermediate representation
([`ir-v1.md`](ir-v1.md)). **Golden** is a deterministic reference image. **Yoga** is the Flexbox
layout engine. **Semantics tree** is the accessibility mirror of the scene graph. See the
[glossary](../reference/glossary.md).

Virtualization keeps only the visible (plus overscan) items realized in the scene graph. It
bounds node count, layout work, paint work, and memory for large lists/grids/charts.

## 1. Scope

In scope:

- vertical/horizontal virtualized lists
- virtualized grids (fixed or measured row/column sizes)
- fixed-size and variable-size items
- keyed recycling and stable identity
- sticky headers/sections (V1: headers, not nested sticky)
- scroll-to-key, scroll anchoring, prefetch/cancellation
- focus/selection/accessibility for realized and unrealized items
- deterministic tests and goldens

Not in scope:

- VM/container/VDI virtualization.
- Virtual DOM. Production is generated C++; the design-time reconciler is separate.
- GPU culling/layer virtualization. Those are rendering concerns; see
  [`dirty-rendering.md`](../design/dirty-rendering.md) and
  [`renderer.md`](../design/renderer.md).
- Infinite nested virtualization, masonry, subgrid, and 3D/virtualized 3D scenes.

## 2. Model

```text
VirtualCollection
  ├─ data source: item count, stable keys, measure/build
  ├─ viewport: scroll offset, extent, overscan, orientation
  ├─ layout: fixed | variable | grid
  ├─ window: realized item range + overscan
  ├─ recycler: keyed node pool
  └─ semantics: set size/index, focus/selection, announcements
```

The collection owns no per-item state for unrealized items. Item state that must survive
scrolling belongs to the data source or an explicit keyed store, not to recycled nodes.
Implemented store: `virtualization::ItemStateStore` in
[`item_state_store.hpp`](../../include/tessera/ui/virtual/item_state_store.hpp) — fixed capacity,
deterministic LRU eviction, no wall clock, no allocation after construction. The data source stays
read-only, `build` never reads recycled state, and state for a key outlives the node that showed
it.

## 3. Data source

Illustrative C++ contract:

```cpp
struct VirtualItemId {
    std::uint64_t value;
};

struct VirtualItemMeasure {
    float main_size;    // height for vertical list, width for horizontal
    float cross_size;   // width/height inside the row/column
};

class VirtualDataSource {
public:
    virtual ~VirtualDataSource() = default;
    virtual std::size_t item_count() const = 0;
    virtual VirtualItemId item_key(std::size_t index) const = 0;
    virtual VirtualItemMeasure measure(std::size_t index,
                                       const MeasureConstraints&) const = 0;
    virtual std::unique_ptr<Node> build(std::size_t index, NodePool&) = 0;
    virtual void release(std::size_t index, std::unique_ptr<Node>) {}
};
```

Rules:

- `item_key(index)` must be stable across inserts/removes/moves.
- `measure` must be pure and deterministic; it may use cached text measurement.
- `build` constructs a node subtree from the item model; it must not read recycled state.
- `release` is optional; the collection may keep nodes in a bounded pool.
- Unknown/duplicate keys are errors.
- Async data loading is allowed; the collection shows a deterministic placeholder until ready.

Implementation: `virtualization::VirtualDataSource` in
[`virtual_data_source.hpp`](../../include/tessera/ui/virtual/virtual_data_source.hpp) keeps
`item_count`/`item_key`/`measure` const (the collection never mutates the source) and resolves
`build` into a caller-owned node that the recycler has just cleared, instead of returning a fresh
`unique_ptr<Node>` as in the illustration above. Filling a pooled node is what makes recycling
avoid one allocation per realized item; the source still writes every field it depends on and
never reads the previous item's state. `virtualization::KeyIndex`
([`key_index.hpp`](../../include/tessera/ui/virtual/key_index.hpp)) snapshots the keys into a
fixed-capacity dense array plus an open-addressed bucket table, so key -> index is O(1) and
allocation-free after construction; a capacity overflow or duplicate key invalidates the whole
mapping instead of serving a partial one.

## 4. Layout and windowing

### Fixed size

```text
stride = item_size + gap
first  = clamp(floor((scroll - overscan) / stride), 0, count)
last   = clamp(ceil((scroll + viewport + overscan) / stride), 0, count)
```

O(1) range calculation; no per-item measurement.

### Variable size

- Maintain a prefix-sum index of item sizes.
- Measure lazily and cache `(key, constraints) -> size`.
- Range = binary search for `scroll - overscan` and `scroll + viewport + overscan`.
- Inserts/removes update the prefix index incrementally.
- Estimate unknown sizes with a default: **32 px main-axis size**. The default is the
  `estimateSize` prop in the section 10 IR example and the constant `kItemEstimateMain` in
  [`item_geometry.hpp`](../../include/tessera/ui/virtual/item_geometry.hpp). There is no
  per-item guess and no growth heuristic, so offsets and totals stay deterministic.
- A real measure replaces the estimate in the prefix sums (`ItemGeometry::refine_measure`):
  only the suffix after the measured key shifts, measured items keep their size and offset, and
  the total extent changes by the delta. The estimate may be restored for an offscreen item
  (`reset_measure`), which shifts the same suffix back.
- Re-measure invalidates only affected suffix/window, not the whole list.

Implementation: `include/tessera/ui/virtual/` + `src/ui/virtual/` (`tessera_virtual`). The
geometry is a fixed-capacity Fenwick tree: `item_offset`, `total_extent`, `index_at_offset` and
`visible_range` are O(log n) and allocation-free; `insert`/`remove` rewrite the shifted suffix
in O(n). Capacity is a constructor argument and a hard cap, not a growth policy.

The collection is `virtualization::VirtualRange`
([`virtual_range.hpp`](../../include/tessera/ui/virtual/virtual_range.hpp)).
`update(scroll_offset, viewport_extent)` clamps the scroll, expands the viewport window by
`ui.virtualListOverscanPx`, releases realized nodes that left the window, acquires and builds the
entering ones, measures each newly realized item, re-anchors the scroll after any refinement that
lands before the viewport anchor, and places the realized nodes on the main axis in viewport space
(the range itself owns the scroll offset; the caller applies it to its scroll container).
The pool is bounded by `ui.virtualListPoolCap`: a window wider than the pool realizes the prefix
that fits and reports `VirtualRangeUpdate::truncated` instead of growing or evicting visible nodes.
Once the pool is warm the whole bookkeeping path is allocation-free (fixed-capacity geometry, key
index, realized table and free ring); a data source may still allocate inside `build`.

### Grid

- V1: fixed column count or fixed column width; row-major.
- Row height = max item height in the row (or fixed row height).
- Variable row heights use the same prefix-sum approach at row granularity.
- Item span/masonry are not in V1 (no release specified).

Implementation: `virtualization::VirtualGridRows` in
[`grid_rows.hpp`](../../include/tessera/ui/virtual/grid_rows.hpp) keeps the fixed-column, row-major
mapping (`row = index / columns`, `column = index % columns`, short last row) and an
`ItemGeometry` at row granularity. A row's extent is the maximum main size of its measured items
(rows with no measured item use the estimate), so re-measuring an item can shrink a row and only
the rows after it shift; `Config::fixed_row_extent > 0` selects the fixed-row-height variant.
`visible_items` maps a visible row window back to the item window, and a count shrink drops the
removed items' measurements so growing again starts from the estimate instead of a stale height.
Storage is allocated once in the constructor: the row window, offsets, and total are
allocation-free, and measurements are O(columns) per item.

### Orientation and RTL

- `orientation: vertical|horizontal`; scroll axis and cross axis are explicit.
- RTL flips the logical start/end for horizontal lists; vertical lists keep physical top/bottom.
- Scroll offset is always physical along the axis; logical alignment is mapped at the API edge.

## 5. Recycling

- Pool keyed by `(item_template, node_kind)`; not by item key.
- On recycle: cancel animations, release pointer capture, clear focus/selection refs, unbind
  event handlers/data refs, reset transient state.
- Do not recycle an item with an active pointer capture, IME composition, or running animation;
  defer until the capture is released, the composition ends, and the animation finishes.
- Reusing nodes is an optimization; correctness must be identical to rebuild.
- Node count is bounded by `visible + overscan + pool_cap`; `pool_cap` is configurable (`ui.virtualListPoolCap` in
  [`buffer-config-v1.md`](buffer-config-v1.md)).
- Keyed identity is preserved for focus/selection/scroll anchoring even when the node is recycled.

Implementation: `virtualization::NodeRecycler` in
[`node_recycler.hpp`](../../include/tessera/ui/virtual/node_recycler.hpp) owns at most
`ui.virtualListPoolCap` node slots plus a FIFO free ring allocated once in the constructor.
`acquire` clears the node before handing it out ("clean on acquire"); `release`/`reclaim` assert in
debug that the node no longer carries item state (`node_carries_item_state`), then sanitize
defensively so release builds keep the same guarantee. `detach`/`reclaim` transfer ownership to and
from the scene, and a generation counter rejects stale leases instead of recycling a node that now
belongs to another item. Once every slot is in use, `acquire` returns `std::nullopt` and changes
nothing: the cap is a refusal, not a growth policy, and the realized window truncates.

## 6. Scroll anchoring and updates

- Maintain an **anchor**: `(item_key, offset_within_item)` near the viewport start.
- On insert/remove before the anchor, adjust `scroll_offset` so the anchor stays visually
  stable.
- On item measurement changes, preserve the anchor unless the anchor item itself changed size;
  then clamp to the nearest valid position. Concretely: an item measured before the anchor shifts
  `scroll_offset` by the same delta, a measured anchor item keeps the offset, an item after the
  anchor changes nothing, and the result is clamped to `[0, total_extent - viewport_extent]`
  (`virtualization::reanchor_scroll`).
- On data replacement, match by key; missing anchor -> nearest surviving key.
- On a geometry change (a prop/state-driven `estimateSize`/`gap`/`itemSize`/`rowHeight`, section
  10), preserve the anchor the same way: the item at the viewport start keeps its
  viewport-relative offset through `reanchor_scroll` with the old/new prefix sums, the result is
  clamped to `[0, total_extent - viewport_extent]`, and only that clamp can change the first
  visible item (the data source is untouched, so its key still exists). Keyed focus/selection are
  unaffected because they are keyed, not positional; `onEndReached`'s edge/throttle state is not
  reset.
- `scroll_to_key(key, align)` realizes the target, measures if needed, scrolls, then focuses if
  requested.
- `scroll_to_index(index, alignment)` is a pure query (`Start`/`Center`/`End`/`Nearest`): it
  computes the offset that puts the item main start at the viewport start, the item center at the
  viewport center, the item main end at the viewport end, or the shortest move that makes the item
  fully visible (`Nearest` does not move an item that is already fully visible). The result is
  clamped to
  `[0, total_extent - viewport_extent]`, and the query never syncs, releases, or rebuilds anything,
  so it cannot cause a full-list resync. Offsets come from the current prefix sums (estimates for
  unmeasured items), so the caller runs `update` with them and uses the returned offset: measuring
  items before the viewport anchor shifts the scroll by the same delta, which can move a target
  placed with `Nearest`; repeating the query with the settled offset is the documented second pass.
  `focus_item`/`step_focus` (section 7) do that pass internally where the alignment matters.
- Sticky headers: a header is pinned at the viewport edge for its section; it participates in
  hit testing and accessibility but is not counted as a regular item.
- No full-list rebuild for a single insert/remove/move.

## 7. Focus, selection, accessibility

- Only realized items exist in the scene graph; the semantics tree exposes a virtual collection
  with `set_size` and per-item `pos_in_set`.
- Keyboard navigation can target an unrealized index: realize + scroll + focus, then announce.
- Focus/selection are keyed by item id, not node pointer; recycling must not move focus to a
  different item.
- Selection ranges may include unrealized items; they are stored by key.
- Screen-reader actions (`scroll_to_index`, `activate`) are handled by the collection.
- Announce data updates at a bounded rate; do not announce every streaming tick.
- Empty/loading/error states are part of the collection semantics, not a fake item.

Implementation: the keyed bits live in `virtualization::ItemStateStore`
([`item_state_store.hpp`](../../include/tessera/ui/virtual/item_state_store.hpp)):
`set_focus`/`clear_focus` keep at most one focused key, `set_selected`/`toggle_selected` maintain
the set, and `ItemState::focused`/`selected` mirror them on the entry (a `put` of payload/measure
state never moves either). The focused key and every selected key are pinned against the store's
LRU eviction, so focus and selection survive scrolling, recycling, and key-preserving data updates.
Capacity therefore bounds the selection: a selection insert beyond it is refused and counted in
`stats().refusals` instead of evicting a selected key, and `VirtualRange::sync` drops entries
(and with them focus/selection) for keys the data source no longer has. `VirtualRange::focus_item`
is the "realize + scroll + focus" path, `step_focus(Next|Previous|First|Last)` is Tab/arrow
stepping (clamped, no wrap, and the first visible item when nothing is focused), `focused_node()`
reports the node currently showing the focused key, and `selected_items(span)` returns the selected
keys in data order (ascending current index). Both mirror the focused key onto exactly one realized
node through `PseudoState::Focus`; releasing the item clears the bit with the node, and realizing it
again restores the bit from the key.

Collection semantics: `virtualization::CollectionSemantics`
([`collection_semantics.hpp`](../../include/tessera/ui/virtual/collection_semantics.hpp)) is the
accessibility/automation view over one range. It reports `set_size()` (the current item count) and
`pos_in_set(index|key)` (1-based, the item's current data index resolved through the key index, so
positions renumber on insert/remove and a key gap is not a position gap), the realized items in
data order with their positions, the realized focused item as the active descendant, the selection
in data order with positions, and a bounded announcement ring. Each item's automation identity is
`(ui_id, item_key)`: `ui_id` is the `automation_id` the compiler assigned to the realized item node
and `item_key` is the stable data key; the pair is the identity and the collection never mints,
renumbers, or re-hashes an id at runtime ([`automation-ids-v1.md`](automation-ids-v1.md) section 1).
An unrealized item has no active-descendant entry and reports `ui_id == 0`; its position still
resolves from the key index.

Announcements are edge-triggered rather than per frame: the host calls `observe()` once after
input/focus handling and drains with `take_announcement()`. A focused-key change or a selection
membership change stages one structured announcement carrying `pos_in_set`, `set_size`, and whether
the item was realized when the change was observed; an undrained announcement of the same kind is
superseded in place (counted in `stats().superseded`), and the ring is bounded by
`kMaxPendingAnnouncements`, dropping its oldest entry (counted in `stats().dropped`) instead of
growing. `observe()` on an unchanged range stages nothing, so focus does not re-announce on every
frame and realizing an unrealized focused item later does not announce again; a focus that moves
away and back between observations nets out to no announcement. Clearing focus names no item and is
counted but not announced. An unrealized-item focus (`focus_item` on a key outside the realized
window, for example a pool-cap truncation or a zero-extent viewport) still announces exactly once
with its position and `realized == false`. The announce path performs no heap allocation after
construction; `format_announcement` builds deterministic developer/debug text into a caller buffer,
while platform bridges localize from the structured fields. `ItemStateStore` keeps the
`selection_revision`/`last_selection_change` journal that lets `observe()` detect a membership
change without copying the selection.

## 8. Async, prefetch, cancellation

- Prefetch data/measure for items just outside the overscan (bounded). Pool cap/overscan
  defaults and user tunables are in [`buffer-config-v1.md`](buffer-config-v1.md).
- Cancel prefetch for items that leave the window.
- Completion callbacks are delivered on the UI thread via the bounded completion queue
  ([`async.md`](../design/async.md)).
- Never block scrolling on data/measure; show placeholders.
- Budget prefetch work per tick; no unbounded queues.
- If an item is recycled before its async result arrives, discard or cache by key.

## 9. Dirty and render integration

- Scroll: update transforms/paint for realized items; do not mark root layout dirty unless
  sticky headers/scroll-linked layout require it.
- Realize/recycle: layout/paint dirty for affected items only.
- Data update: mark changed items dirty; insert/remove updates layout/prefix index.
- Measurement change: relayout from the affected item; preserve anchor.
- `dirtyRendering=off` ([`runtime-config-v1.md`](runtime-config-v1.md) section 5.4) must produce identical pixels to dirty-only for the same viewport.
- Virtualization does not replace GPU culling/layer caches; it reduces realized nodes.
- Goldens always full-redraw the realized set into a fixed target.

## 10. IR and component model

IR adds two collection node kinds (see [`ir-v1.md`](ir-v1.md)):

- `VirtualList`
- `VirtualGrid`

Illustrative IR:

```json
{
  "id": "node::rows",
  "kind": "VirtualList",
  "props": {
    "estimateSize": 32,
    "overscan": 200,
    "gap": 4,
    "stickyHeaders": true
  },
  "events": { "onEndReached": "bind::on_end_reached" },
  "itemTemplate": {
    "id": "node::row",
    "kind": "View",
    "key": { "kind": "ref", "id": "row.id" },
    "loop": { "kind": "loop", "item": "row", "index": "i", "source": { "kind": "ref", "id": "state::rows" } },
    "style": { "display": "flex", "padding": "4px 8px" }
  }
}
```

Rules:

- The item template is a static IR subtree/component; dynamic component types are rejected.
- The data source is the template's `loop.source` (a list state/ref); item access uses the loop's `item`/`index` locals ([`ir-v1.md`](ir-v1.md) section 3.5).
- `key` is required; duplicate keys are errors.
- V1 supports vertical lists, horizontal lists, and fixed/measured grids; span/masonry are not in V1 (no release specified).
- `onEndReached` is throttled and delivered on the UI thread. The throttle is distance-based and
  deterministic (no wall clock): the event is edge-triggered when the realized window (viewport +
  overscan) first reaches the last item, staying at the end does not re-fire, and a re-entry whose
  scroll offset is within `kVirtualEndReachedThrottlePx` (200 px) of the previous fire is suppressed
  and counted. The edge re-arms only after the window leaves the end zone.
- `onScroll` and `onEndReached` are collection-level events. `onScroll` accepts one parameter and
  maps it to the scene `ScrollEventInfo` payload (absolute offset plus viewport/content extents);
  `onEndReached` accepts no parameter. Both are dispatched by the generated
  `scroll_virtual_<n>(float& scroll_offset, float viewport_extent)` entry point (docs/design/
  compiler.md section 11), which owns an `ItemGeometry` seeded from the emitted geometry values
  (re-seeded by `update_virtual_geometry_<n>` when a prop/state input changes), forwards realized-item
  measures through `refine_measure`, re-anchors the caller's offset in place with the section 6
  rule, and applies the overscanned window. A source past the generated geometry cap (4096 items) is
  refused by the entry point. An `onEndReached` parameter, an `onScroll` payload other than
  `ScrollEvent`, a multi-parameter collection handler, a collection-event binding with a return
  value, or a collection event on a `VirtualGrid` without a `columns` literal or prop is a
  compile-time error.
- Geometry inputs are dynamic: `estimateSize`, `gap`, `itemSize` (list) and `rowHeight` (grid)
  accept a number literal **or** a numeric state/prop reference. The generated
  `update_virtual_geometry_<n>(float& scroll, float viewport)` re-seed path re-evaluates them,
  hash-compares them (one FNV-1a pass, no allocation) and seeds once per change, so a per-frame call
  never rebuilds an unchanged geometry. Contract on a change:
  - an `estimateSize` or `gap` change **keeps** every measurement
    (`ItemGeometry::reseed(estimate, gap)`): an estimate only describes items never measured, and a
    gap moves offsets rather than an item's own size;
  - a change to a declared fixed extent that seeds the estimate (an `itemSize`/`rowHeight` with no
    `estimateSize` overriding it) **drops all** measurements, because that extent describes every
    item; a fixed extent the estimate overrides is informational and never drops one;
  - the item at the viewport start keeps its viewport-relative position: `reanchor_scroll` with the
    old/new prefix sums, clamped to `[0, total_extent - viewport_extent]`. A geometry change does not
    touch the data source, so that item's key survives and only the clamp can move the first visible
    item;
  - `onEndReached`'s edge/throttle state neither resets nor fires; a moved content end may leave the
    end zone, which re-arms the edge through the ordinary rule.
  `orientation`, `columns`, `overscan` and `columnWidth` accept either a literal **or** a component
  prop reference (`orientation={props.orientation}`, `columns={props.columns}`). A literal is still
  emitted as a `static constexpr` (byte-identical output). A prop-driven value is a mutable
  `virtual_<n>_*` member that `scroll_virtual_<n>()` refreshes from the generated props field at the
  start of each sync, before the row/window math, so a prop change re-realizes the collection on the
  next sync: the `ItemGeometry::Range`, the grid row count, the measured main axis and the
  overscanned window all use the new value. The item table is reused by key, so unrelated nodes keep
  their `NodeId` and layout, and the refresh path allocates nothing. Only a `prop::` reference is
  accepted for these four options; a state/animated reference, a computed expression (a call or an
  arithmetic expression) and a prop of the wrong type are `TESSERA_CODEGEN_UNSUPPORTED` errors that
  name the option and the reason. Runtime contract for a prop value (no crash): an `orientation`
  string other than `horizontal` is ignored (vertical, the default axis); an `overscan` below 0 is
  clamped to 0; a `columns` value below 0 is clamped to 0, the column-width mode in which
  `scroll_virtual_<n>()` refuses the generated row mapping (`return false`, the host drives
  `apply_virtual_<n>` with its own grid rows) and leaves the row geometry untouched. The re-seed path
  is allocation-free and deterministic (no wall clock, no pointer order); the emitted `virtual_<n>_*`
  members hold the last seeded values.
- IR v1 gains these node kinds; codegen emits a fixed container node, generated geometry constants
  (estimate/fixed size, overscan, gap; grid row height/columns), a bounded realized-window item
  table and `apply_virtual_<n>(first, last)` (ir-v1.md section 10). The host computes `[first, last)`
  with the `tessera_virtual` geometry and owns the scroll offset; the generated path does not
  reimplement prefix sums. The item key is the template's `key` expression, item nodes take
  `(template NodeId, absolute item index)` identity, and an unchanged window or unchanged item
  content marks nothing dirty.

## 11. Determinism and tests

Deterministic inputs:

- fixed viewport size, scroll offsets, item sizes, data order
- committed fonts for text measurement
- no wall clock, network, or random ids
- deterministic recycle/pool order

Tests:

- fixed/variable/grid/horizontal/RTL range calculation
- insert/remove/move before/after the anchor; no scroll jump
- scroll-to-key, sticky headers, end-reached throttle
- focus/selection survive recycle and data updates
- accessibility set size/pos and unrealized-item focus
- async prefetch/cancel/recycle; no stale callback
- dirty-only == full-render pixels; no node leak after long scroll
- golden scroll scripts (viewport snapshots + realized range report)

## 12. Performance budgets

- Realized nodes <= visible + overscan + pool cap.
- Scroll update should be O(visible), not O(item count).
- No per-scroll allocation after warm-up.
- Measure cache bounded by bytes/items; evict offscreen entries.
- Prefetch bounded per tick; cancellable.
- Track: realized range, recycled count, measure cache hit/miss, prefetch queue depth,
  scroll frame time, pool size.

## 13. Open questions

- Variable-size estimation strategy and correction when estimates are wrong. **Decided:** a
  constant 32 px main-axis estimate, replaced by a real measure; only the suffix shifts and the
  anchor re-anchors (sections 4 and 6). No correction heuristic runs on top of a real measure.
- Grid span/masonry and variable column widths.
- Nested virtualization (virtual list inside a virtual row).
- Whether item state should live in a keyed store or in the data source. **Decided:** an explicit
  keyed store owned by the component (section 2); the data source stays read-only and recycled
  nodes hold no item state.
- Selection size versus the item-state store capacity. **Decided:** focused and selected entries are
  pinned, so selection is bounded by capacity and a selection insert beyond it is refused (never
  silently evicted); the component sizes the store for `selection + focus + payload` entries
  (section 7).
- Recycling vs rebuild for complex/custom item trees.
- IR representation: decided in [`ir-v1.md`](ir-v1.md) (`VirtualList`/`VirtualGrid` node kinds, closed in V1); not an open question.
- Sticky headers/sections with variable heights.
- Async data source paging vs in-memory list.
- Accessibility navigation for very large collections.
- Horizontal RTL scroll anchoring edge cases.

## 14. Status

- Implemented (B2 slice, `tessera_virtual`): main-axis prefix-sum geometry with 32 px estimates,
  estimate-to-measure refinement that shifts only the suffix, visible-range/total-extent/
  index-at-offset lookup, and the section 6 re-anchor math
  (`include/tessera/ui/virtual/`, `src/ui/virtual/`); bounded keyed item state store with
  deterministic LRU eviction (recycled nodes hold no state); the read-only `VirtualDataSource`
  adapter contract with a clean-node `build` and optional `release`; the fixed-capacity
  `KeyIndex` key -> index resolver (capacity overflow and duplicate keys invalidate the mapping);
  and the bounded `NodeRecycler` (cap refusal, clean-on-acquire, debug no-item-state assertion,
  FIFO reuse, `detach`/`reclaim` scene hand-off, stale-lease rejection); and `VirtualRange`, the
  visible-range integration over the geometry (overscanned window, build/release churn, measure on
  realize, `reanchor_scroll` after a refinement before the anchor, main-axis placement, cap
  truncation, and an allocation-free scroll path after pool warm-up).
- Implemented (keyed focus/selection slice): the focused key and the selection set live in
  `ItemStateStore` keyed by `VirtualItemId` and pinned against LRU eviction
  (`set_focus`/`clear_focus`/`is_focused`, `set_selected`/`toggle_selected`/`selected_count`/
  `copy_selected`, `retain_if` pruning on `sync`); `VirtualRange::focus_item` realizes, scrolls and
  focuses a key, `step_focus(Next|Previous|First|Last)` is clamped Tab/arrow stepping that leaves
  exactly one realized node carrying `PseudoState::Focus`, `focused_node()` reports the realized
  node, `selected_items(span)` returns keys in data order, and `scroll_to_index` adds
  `Start`/`Center`/`End`/`Nearest` placement with the documented clamp and no resync.
- Implemented (semantics/accessibility slice): `virtualization::CollectionSemantics` exposes the
  set size and 1-based `pos_in_set` resolved through the key index, realized items in data order
  with their `(UiId, ItemKey)` automation identity (read from the realized node's `automation_id`,
  never re-hashed), the realized focused item as the active descendant, selection in data order with
  positions, and bounded coalesced focus/selection announcements (one per observed change, none per
  frame, superseded entries dropped in place, allocation-free after construction).
  `ItemStateStore` gained the `selection_revision`/`last_selection_change` journal that
  `observe()` consumes.
- Implemented (grid slice): `VirtualGridRows` maps the fixed-column, row-major grid onto an
  `ItemGeometry` at row granularity, with max-item-height (or fixed) row extents, row/item visible
  windows, capacity refusal, and removal-aware measurements.
- Implemented (compiler slice): the `VirtualList`/`VirtualGrid` `.map` child lowers into the IR
  `itemTemplate` (with the collection's 32 px `estimateSize` default, ir-v1.md section 3.5), and
  codegen emits the item-template builder plus the bounded `apply_virtual_<n>(first, last)` window
  path (docs/design/compiler.md section 11). The host computes the window with `VirtualRange` /
  `ItemGeometry`, so generated code never reimplements prefix sums. Goldens
  `tests/golden/codegen/VirtualDemo.*`; tests `codegen_virtual` (shape, goldens, determinism,
  rejections, merger idempotency), `codegen_virtual_runtime` (a scripted scroll through the real
  `ItemGeometry`), and `codegen_virtual_scroll` (the generated scroll entry point: overscanned
  range parity, measure refinement and re-anchor, the edge/throttle `onEndReached`, the grid row
  window, and the geometry-cap refusal).
- Implemented (collection events slice): `onScroll`/`onEndReached` are accepted on `VirtualList` and
  `VirtualGrid` and dispatched by the generated `scroll_virtual_<n>` entry point, which owns the
  collection's `ItemGeometry` seeded from the emitted geometry values, refines the prefix sums from
  the realized items' measured main-axis bounds (`ItemGeometry::refine_measure`, grid rows keep their
  maximum), re-anchors the caller's offset in place (`reanchor_scroll`), realizes the overscanned
  window, and applies the documented distance throttle to `onEndReached`.
- Implemented (dynamic geometry slice): `estimateSize`/`gap`/`itemSize`/`rowHeight` accept a number
  literal or a numeric state/prop reference, and the generated `update_virtual_geometry_<n>` re-seed
  path re-evaluates them behind an FNV-1a hash guard, so it seeds once per change and never rebuilds
  a per-frame call. `ItemGeometry::reseed` adopts a new estimate/gap while keeping every measurement;
  a changed declared fixed extent that seeds the estimate additionally calls `clear_measurements`.
  The offset re-anchors with the old/new prefix sums and the section 6 clamp, so the viewport-start
  key keeps its position (or the offset clamps to `[0, total - viewport]`), and `onEndReached`'s
  edge/throttle state is untouched. Codegen rejects a non-numeric cell, a computed geometry
  expression, and a state-driven structural option. Goldens
  `tests/golden/codegen/VirtualDemo.*`; tests `codegen_virtual_geometry` (emission and rejection
  matrix, determinism) and `codegen_virtual_geometry_runtime` (one seed per change, measurements kept
  vs dropped, the preserved anchor, the clamp, the untouched throttle).
- Implemented (prop-driven structural options slice): `orientation`/`columns`/`overscan`/
  `columnWidth` accept a component prop reference as well as a literal. A literal keeps the exact
  `static constexpr` text (the `VirtualDemo.*` golden does not move); a prop-driven option is a
  mutable generated member refreshed by `scroll_virtual_<n>()` at the start of the sync, so a prop
  change re-realizes the window on the next sync (new grid row count and `ItemGeometry::Range`, new
  measured axis) without rebuilding the component tree or changing an unrelated node's `NodeId`
  (the realized-window table is reused by key). A negative prop `overscan` clamps to 0 (no overscan),
  a non-positive prop `columns` clamps to the column-width mode 0 (the generated row mapping refuses
  and the host drives `apply_virtual_<n>`), and an unknown prop `orientation` falls back to vertical;
  none of them crash. Only a component prop reference is accepted: a state/animated reference, a
  computed expression and a wrong prop type are typed `TESSERA_CODEGEN_UNSUPPORTED` errors. Goldens
  `tests/golden/codegen/dynamic/DynamicVirtual.*`; test `codegen_dynamic_virtual_options` (emission
  shape, literal snippet and unchanged literal golden, the rejection matrix, and the runtime
  re-realization/validation cases).
- Tests: `virtual_geometry` (forward/backward scroll, estimate-then-measure totals, mid-list
  refine suffix shift, insert/remove, capacity), `virtual_geometry_scan` (brute-force linear
  oracle for offsets and windows), `virtual_anchor` (re-anchor after measure, clamp),
  `virtual_state` (state across recycle, eviction rule), `virtual_allocations` (no heap
  allocation on the scroll/refine/state path), `virtual_data_source` (read-only surface, measure
  determinism, build/release contract), `virtual_key_index` (stable keys across insert/remove,
  duplicate/capacity errors, deterministic collision handling), `virtual_pool` (cap enforcement
  without mutation, clean-on-acquire/no-state-leak, FIFO order, lease validation, allocation-free
  pooled acquire/release), `virtual_range` (realize/release churn, reuse, cap truncation,
  horizontal placement, key resolution/scroll-to-key, sync invalidation, error paths),
  `virtual_range_measure` (re-anchor before/at/after the anchor, clamp, node bounds),
  `virtual_focus` (focus survives scroll-out/recycle and is restored on realize, realize-on-focus,
  Tab/arrow stepping with exactly one realized focused node, selection data order, focus/selection
  across a key-preserving data update and a key removal, pool-truncation and zero-viewport focus),
  `virtual_scroll_index` (Start/Center/Nearest placement, clamping, pure-query/no-resync, measured
  prefixes, and the re-anchor interaction with focus), `virtual_focus_churn` (10k-item
  allocation-free focus/selection/step churn, exactly one focused node, deterministic step and
  build/release order), `virtual_grid_rows` (row/column mapping, short last row, max-item row
  extents, fixed row height, row/item windows, capacity refusal, removal-aware measurements),
  `virtual_collection_semantics` (set size/`pos_in_set` matrix with key gaps and removals, realized
  exposure and `(UiId, ItemKey)` identity, active descendant across realize/recycle, selection data
  order, allocation parity, determinism), `virtual_announce` (one announcement per observed change,
  supersede/drop coalescing, unrealized-item focus with its position, FIFO across kinds, no
  per-frame announcement, allocation parity, determinism), and `virtual_churn` (10k-item
  allocation-free churn, no node leak, deterministic step records/build/release order).
- Design/spec only still: sticky headers, the platform accessibility bridge consuming the
  collection semantics, async prefetch/cancellation wiring, a scene `Scroll` event binding for the
  collection events (the host calls the generated `scroll_virtual_<n>` entry point today), and
  dirty/render integration beyond the realized-window path (the range reports
  dirty through the scene-node bounds it writes).
- Depends on: IR/component model ([`ir-v1.md`](ir-v1.md),
  [`component-model-v1.md`](component-model-v1.md)), Yoga layout/measure
  ([`layout.md`](../design/layout.md)), input/focus ([`input.md`](../design/input.md)),
  accessibility ([`accessibility-v1.md`](accessibility-v1.md)), dirty rendering
  ([`dirty-rendering.md`](../design/dirty-rendering.md)), async ([`async.md`](../design/async.md)).
- Target: Big Step B2 for basic list virtualization; Big Step E for large streaming dashboards.
