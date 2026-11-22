# CPU cache-line layout

Status: design. Companion to [`performance.md`](performance.md) section 3 (budgets) and
[`threading-and-memory.md`](threading-and-memory.md). Records findings C1-C8 of the security review
([`security-review-v1.md`](../specs/security-review-v1.md) section 12.2).

A cache line is 64 bytes on every lane this project builds for. Two kinds of defect follow from
that, and neither shows up in a functional test:

- **False sharing.** Two atomics that *different threads* write, in the same line, make every write
  invalidate the other thread's copy. The counters stay correct, so no test fails and no profiler
  points at it; the cost is coherence traffic on exactly the paths the runtime is designed to keep
  cheap. The fix is to group counters by *writer*, not by meaning, and give each group its own line.
- **Straddling.** A hot struct whose size is a multiple of 64 but whose alignment is smaller starts
  at an arbitrary offset, so most instances span two lines and every sequential write touches both.

## 1. The mechanism

`include/tessera/detail/cache_line.hpp` is the one mechanism: `detail::CacheLine<T>` is `T` padded
to exactly 64 bytes and aligned to it, so a group fills one line and the alignment never forces a
hole (which is also what keeps MSVC's C4324 quiet).

The line size is a fixed 64 rather than `std::hardware_destructive_interference_size`, because the
standard's value is not usable in a `static_assert` on every supported toolchain (GCC warns that it
is not an ABI constant). Over-aligning on a 128-byte-line machine costs a little padding and is
never wrong; under-aligning would silently reintroduce the sharing.

## 2. The groups that exist today

| Type | Groups | Why |
|---|---|---|
| `runtime::WorkerIoExecutor` | UI (`posted`, `rejected`, `peak_in_flight`, `stopping`), worker (`started`, `completed`, `failed`, `completions_dropped`), shared (`in_flight`) | The eight counters were 64 bytes declared consecutively - one line, not even aligned - so every submission and every completion invalidated the same line, and the `in_flight` CAS sat in the middle of it |
| `runtime::IoExecutor` | UI (`submitted`, `rejected`, `peak_in_flight`), worker (`completed`, `failed`, `cancelled`), shared (`in_flight`) | Same defect, seven counters |
| `ui::InputQueue` | producer (`tail`, `overflowed`), consumer (`head`) | The ring is single-producer/single-consumer; today every producer runs on the UI thread, so this is latent, but [`input.md`](input.md) section 7 anticipates a platform callback thread |
| `diagnostics::MetricsRegistry` | hot (`std::atomic<std::uint64_t>` array), cold (`SlotName` array) | Interleaved, a slot was a 32-byte name plus the 8-byte counter, so counters sat at a 48-byte stride - 1.33 per line - and every increment dragged in a name it never reads. Split, a line holds eight counters |
| `gpu::Vertex2D` | `alignas(64)` on the struct | Exactly 64 bytes but naturally 4-aligned, so three of four vertices straddled two lines and the vertex stream paid double store traffic |

## 3. Struct padding

Two hot types carried avoidable internal padding from incremental member order, and both are now
grouped by alignment:

| Type | Before | After | What was wrong |
|---|---|---|---|
| `ui::Node` | 312 | 304 | One byte after `focus_group_` and seven after `scroll_bits_`, because 1-byte members landed between 4-byte and 8-byte ones |
| `ui::InputEvent` | 344 | 320 | Seven bytes after `type` to align the `double`, three after `pointer_kind`, four of tail padding; the 256-event queue copies it on every push and pop |

Both are pinned by the `sizeof_node` and `sizeof_input_event` rows of
[`tests/golden/perf/memory_budget.json`](../../tests/golden/perf/memory_budget.json) under the
`enforce` class, so a regression fails the gate rather than showing up as a slow trend.

## 4. How it is pinned

`tests/unit/cache_line_tests.cpp` asserts every invariant above with `static_assert`s, so a
regression is a build failure rather than a slow test. The rules for new code:

- A new counter that two threads write belongs in a group, not beside the others.
- A new hot struct whose size is a multiple of 64 needs `alignas(64)`.
- A struct that grows past its budget needs a recorded decision, the same rule as
  [`performance.md`](performance.md) section 3.1.

## 5. The types that landed before the rule (issue #732)

The rule above was written after five types had already landed without it. Each has a size that is
a multiple of 64, so with the natural 8-byte alignment seven of eight instances straddle a line and
a sequential walk touches both. They now carry `alignas(64)`, and `cache_line_tests.cpp` asserts
both halves (size is a multiple, alignment is the line) for each:

| Type | sizeof | held in |
|---|---|---|
| `ui::InputEvent` | 320 (5 lines) | the 256-event ring, copied on every push and pop |
| `ui::style::Rule` | 64 | `std::vector<Rule>`, every parsed rule |
| `ui::style::MediaQuery` | 64 | `std::vector<MediaQuery>` |
| `ui::style::ThemeToken` | 64 | theme token and declaration lists |
| `ui::style::ThemePin` | 64 | the theme pin list |

`alignas` changes no offset, so the `sizeof_input_event` budget row and `kInputEventSizeBudget` are
unaffected: the cost is the tail padding a vector's allocator rounds up to, not a wider element.

**False sharing in `UiaHost`.** Its five atomics used to sit in one 64-byte line, so the UI thread's
`mark_dirty`/`tick` and the bridge thread's `flush_events` invalidated each other's copy on every
publish and every flush. They are now two `detail::CacheLine` groups, split by writer: `dirty` and
`flush_posted` for the UI thread, `root`, `raised_batches` and `failed_raises` for the bridge thread.
`cache_line_tests.cpp` asserts the host holds at least two separate groups.

`AtspiSession`'s `phase_`/`error_`/`running_` share a line, but a start/stop is a once-per-session
event rather than a per-frame one, so that is recorded as accepted rather than fixed.

## 6. `ui::Node`: locality, not sharing (god-type audit step 3)

`ui::Node` is written by one thread, so false sharing is not its problem. Its cost is locality: a
whole-scene pass (the dirty pass, the hit-test descent, the paint cull walk) reads a few fields of
every node, and those fields used to sit on four of the node's five lines. The work followed the
measurements, in this order, because the first two passes were algorithmic and the layout only
matters once they are fixed.

**Method.** `tessera_benchmark --walk [rounds]` builds the 12 000-node reference scene, runs one
full frame so it is laid out, then times three passes with `perf_event_open` counters around each:
a hot-field walk (the lower bound of any traversal), the dirty `collect + clear` over a fully dirty
tree, and `hit_test_deep` over a fixed 16 x 10 point grid. It is a report, not a gate (no budget,
never fails). It needs `kernel.perf_event_paranoid <= 1` for the counters; without that the time
columns still print and the counters read `n/a`. Numbers below are Linux Release GCC, one pinned
core, three runs within 2%; the hit checksum is identical in every row (same hits, different cost).

| Step | Hot walk ns/node (L1D miss) | Dirty ns/node (L1D miss) | Hit test us/point |
|---|---|---|---|
| Baseline | 12.7 (not counted) | 840 | 3 200 |
| `hit_test_deep` pruned + one inversion | 12.9 (3.8) | 800 (562) | 1.3 |
| Dirty clear in one post-order pass | 12.9 (3.8) | 24 (6.5) | 1.3 |
| Heap nodes on a cache line | 9.7 (3.1) | 19.5 (5.4) | 1.3 |
| First line holds the hot fields | 10.3 (2.2) | 20.8 (4.6) | 1.0-1.15 |

**Reading it.** Alignment is the large time win (about -25% on the walk). The layout change cuts L1D
misses a further 30% and speeds the hit test about 20%, but the walk and the dirty pass are within 5%
of the aligned numbers, slightly slower: the 3.8 MB tree sits in L2/LLC, so an L1D miss is cheap and
overlaps, and the node's children live in a separate vector buffer, so a visit is bounded by the
pointer chase as much as by lines. The layout is kept because it removes the misses structurally and
is pinned, not because this benchmark's wall time proves it; a larger or colder tree is where it
should pay, and is not measured here.

**What is on the first line.** `sizeof(Node)` is 320 (five lines) with no padding. Offsets 0..63:
the vptr, `parent_`, `children_`, `bounds_`, the flag word, both dirty summaries, `scroll_bits_`,
`user_select_`, `cursor_` and `focus_group_`: 8 + 8 + 24 + 16 + 2 + 1 + 1 + 1 + 1 + 1 + 1 = 64. The
36-byte transform and the z-index do not fit, so two flag bits (`kHasTransform`, `kHasZIndex`) stand
for them: the hit-test descent and the sibling walk read the bit and touch the value only when it is
set (most nodes have neither). Everything else is on demand from offset 64: the cached subtree
paint box, the transform, z-index, tab index, pseudo state, paint outsets, glass sigma, the scroll
floats, the id, and the 120-byte metadata record.

**Alignment.** Heap nodes start on a line through a class-level `operator new` (64-byte aligned
allocation), not through `alignas(64)` on the type as section 4 prescribes for other hot structs.
`Node` has many derived classes (every generated component, every test recorder); an over-aligned
base pads each of them and trips MSVC's C4324 on each. Stack and by-value nodes are not aligned,
which costs speed and never correctness. The default aligned `operator new` does not call
`operator new(std::size_t)`, so every allocation-counting test replaces the aligned forms too
(`detail/aligned_alloc.hpp`); a node allocated inside a counted region is still counted.

**How it is pinned.** `NodeLayoutCheck` in `node.cpp` asserts, on the 64-bit lanes, that each hot
field ends inside the first 64 bytes and that the first on-demand field starts exactly at 64. The
exact `sizeof(Node) == 320` assert in `node.hpp` is unchanged. A new member on line 0 by mistake, or
a reorder that pushes a hot field across the boundary, is a build failure. `scene_hit_test_prune`
pins the flag semantics (`has_z_index`, `has_transform` follow every write, including back to the
default).
