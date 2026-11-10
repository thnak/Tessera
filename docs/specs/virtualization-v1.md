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
- `scroll_to_key(key, align)` realizes the target, measures if needed, scrolls, then focuses if
  requested.
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
- `onEndReached` is throttled and delivered on the UI thread.
- IR v1 gains these node kinds; codegen emits collection construction/update calls.

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
  `virtual_range_measure` (re-anchor before/at/after the anchor, clamp, node bounds), and
  `virtual_churn` (10k-item allocation-free churn, no node leak, deterministic step
  records/build/release order).
- Design/spec only still: grid rows, sticky headers, focus/selection/accessibility integration,
  async prefetch/cancellation wiring, IR/codegen emission for `VirtualList`/`VirtualGrid`, and
  dirty/render integration (the range reports dirty through the scene-node bounds it writes).
- Depends on: IR/component model ([`ir-v1.md`](ir-v1.md),
  [`component-model-v1.md`](component-model-v1.md)), Yoga layout/measure
  ([`layout.md`](../design/layout.md)), input/focus ([`input.md`](../design/input.md)),
  accessibility ([`accessibility-v1.md`](accessibility-v1.md)), dirty rendering
  ([`dirty-rendering.md`](../design/dirty-rendering.md)), async ([`async.md`](../design/async.md)).
- Target: Big Step B2 for basic list virtualization; Big Step E for large streaming dashboards.
