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
