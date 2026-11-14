# Chart series sample storage v1: fixed-capacity ring buffer

Status: proposed (Big Step E, V1-M6 "Data-heavy UI", [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step);
issue #182). This spec covers **only the series sample storage** that chart nodes and the streaming
dashboard build on. Chart nodes, paint/damage behaviour and the dashboard sample are #183 and #184;
they are not specified here. Rendering context: [`dirty-rendering.md`](../design/dirty-rendering.md)
section 8; component scope: [`component-library.md`](../design/component-library.md).

Terms: **series** is one ordered set of samples drawn as one line/area/bar series. A **sample** is
one point (time, value). **Capacity** is the fixed number of sample slots a series owns.
**Overwrite-oldest** means a full series replaces its oldest sample instead of growing. The
**physical index** is the slot in the backing array; the **logical index** is the position in the
rendered order, where 0 is the oldest sample. A **span** is a contiguous run of physical slots.
See the [glossary](../reference/glossary.md).

## 1. Sample value

```cpp
namespace tessera::ui::chart {
struct ChartSample {
    double timestamp = 0.0; // seconds on the series' own monotonic clock
    float value = 0.0f;     // chart data, passed through unchanged
};
}
```

- `timestamp` is a time in **seconds** on a monotonic time base (normally
  [`runtime::SteadyClock`](../../include/tessera/runtime/steady_clock.hpp)); it is not a calendar
  time and is never compared against wall-clock time. It must be **finite** and **strictly
  increasing** within one series; a renderer may therefore assume sample `n + 1` is to the right of
  sample `n`.
- `value` is the y coordinate. The ring buffer does not validate it and does not interpret it; a
  chart node decides what a non-finite value means (for example a gap). Any rule for gaps is a chart
  node contract, not a series storage contract, and is not decided by this spec.
- `ChartSample` is trivially copyable and is the only per-sample storage the ring has. It carries no
  string, pointer or owning object, so appending and reading never run a destructor, copy
  constructor or allocation.

The sample layout is the V1 decision recorded by #182 (time, value). Extending the payload (OHLC
candles, several values per timestamp, labels) is open; see section 8.

## 2. Capacity

- Capacity is chosen **once at construction** and is fixed for the object's lifetime. It must be at
  least 1; a capacity of 0 is rejected with `std::invalid_argument`.
- Construction allocates exactly the backing storage (`capacity * sizeof(ChartSample)`). It is the
  only allocation the series performs.
- The series never grows, never reallocates and never allocates on append, read, iteration or
  clear. Memory is therefore bounded by construction, independent of how much data streams in.
- Capacity is a property of the chart node or of the app that owns the series, not a new
  `tessera.json` key: this slice adds no runtime or buffer-configuration key. Adding one (for
  example `ui.chartSeriesCapacity`) needs a decision recorded in
  [`buffer-config-v1.md`](buffer-config-v1.md) (section 8, open question).

## 3. Append and overwrite

`append(ChartSample)` returns `true` when the sample was stored and `false` when it was rejected;
it is `noexcept` and never allocates.

- A sample is rejected, leaving the series unchanged, when its `timestamp` is not finite or is not
  **strictly greater** than the current newest sample's timestamp. This is what keeps the logical
  order equal to timestamp order. Equal timestamps are rejected; a producer that needs two points
  at the same instant must offset one.
- While the series is not full, the sample is stored at the next logical position; `size()` grows
  by 1.
- When the series is full, the append **overwrites the oldest sample** (logical index 0), which
  becomes the new newest sample; `size()` stays at capacity. No other sample moves.
- After a series has wrapped, appending an older sample is still rejected: the overwrite rule is
  not a licence to insert out of order. The surviving samples are always the most recent
  `size()` samples in strictly increasing timestamp order.
- The overwrite is silent: the series does not report which sample was evicted. A renderer detects
  a full series from `full()` / `size() == capacity()`.

## 4. Read, iteration and spans

- Logical reads are `series[0]` (oldest) through `series[size() - 1]` (newest). The index must be
  less than `size()`; the checked form exists only in debug builds (`assert`), so Release callers
  own the precondition as with `std::span`.
- `oldest()` and `newest()` read those two samples; they require a non-empty series.
- Forward iteration (`begin()`/`end()`, so range-for) visits samples in logical oldest -> newest
  order, independent of where the physical wrap point is.
- `spans()` returns the same sequence as at most two contiguous runs:
  `[oldest .. physical end]` followed by `[physical start .. newest]`. Concatenating the two spans
  is the logical sequence, with no copy and no allocation. A series that has not wrapped returns one
  non-empty span and one empty span; an empty series returns two empty spans. A renderer may
  therefore stream the samples straight from the spans.
- Spans, samples and iterators are invalidated by `append()` (it can overwrite the sample a pointer
  refers to), `clear()` and a move. A frame may consume them; it must not keep them across a later
  append.

## 5. Clear and reset

`clear()` is `noexcept` and allocates nothing. It sets `size()` to 0 and keeps capacity and
storage; the sample values are not zeroed. The next append starts a new time range and may carry
any finite timestamp, because there is no newest sample to compare against. `clear()` is the reset
for reuse of the same series object (for example when a chart changes data set), not a restart of a
clock domain.

## 6. What a chart series guarantees to a renderer

Given a `ChartSeriesRing` with capacity `N`:

1. **Bounded work and memory.** A full traversal touches at most `N` samples; memory is
   `N * sizeof(ChartSample)` for the ring's lifetime, plus the object itself.
2. **Order.** Samples are in strictly increasing timestamp order; logical index 0 is the oldest and
   `size() - 1` the newest. Timestamps never repeat.
3. **No allocation on the frame path.** Append, indexed read, iteration, `spans()`, `oldest()`/
   `newest()` and `clear()` allocate zero blocks and zero bytes, so a chart node can call them in
   the frame/paint path (never blocking the UI thread, [ADR-021](../../decisions/ADR-021-async-runtime.md)).
4. **No copy needed.** The two spans expose the samples contiguously in render order, so a renderer
   can build vertices by walking them.
5. **Explicit eviction.** Capacity is a hard bound; the series silently drops the oldest samples.
   A chart node that must show the evicted range (a full history zoom-out) cannot recover it from
   the ring; it keeps a lower-rate history itself, or the app keeps one (section 8).
6. **No hidden state.** The ring does not mutate timestamps or values, does not interpolate, does
   not decimate and does not decide axes. Those are chart-node concerns.

## 7. Threading and ownership

A `ChartSeriesRing` is **not thread-safe** and is not designed to be: a series belongs to one chart
node and is updated on the UI thread, exactly like the scene it feeds. A producer on another thread
publishes immutable values through `runtime::DataStore` ([`async-data-v1.md`](async-data-v1.md));
the app copies/appends the samples it wants into the series when it observes the store on the UI
thread. The ring has no lock, no atomic and no wake-up path, so it cannot block the UI thread or
wake the loop.

The ring owns its storage through `unique_ptr<ChartSample[]>`, so it is movable (move is
allocation-free) and not copyable. A moved-from ring is inert: capacity 0, size 0, appends return
false; only destruction, move assignment or a new ring replaces it.

## 8. Open questions

1. **Variable-rate series and decimation.** The ring stores whatever rate the app appends. If a
   producer outruns the chart's pixel width (1M points on 800 px), who downsamples: the app before
   append, the chart node before paint, or a decimating writer? This spec deliberately stores raw
   samples and leaves decimation to #183/#184; it does not define a stride or a resolution policy.
2. **Capacity source.** This slice makes capacity caller-supplied. A default per chart type and a
   `buffer-config-v1.md` tunable are not decided.
3. **Payload extension.** Multi-value samples (OHLC/candlestick), per-sample labels or colors, and
   several aligned series sharing one x axis need a type decision (a richer sample struct, a
   structure-of-arrays ring, or generic ring over a trivially copyable `T`). #182 fixes only
   `(double timestamp, float value)` for line/area/bar V1.
4. **Duplicate timestamps.** Strictly increasing is chosen so a renderer never sees a zero-width x
   segment. If a real stream needs several samples per instant, revisit (offsetting is the current
   answer).
5. **Non-finite values as gaps.** Whether `NaN` means "gap" or "invalid sample" is a chart-node
   concern; not specified here.
6. **Eviction reporting.** An eviction counter or the evicted sample itself is not exposed. Add it
   only if a chart needs it for an axis or a "new data" marker.

## 9. Acceptance

`tests/unit/chart_series_ring_tests.cpp` and `tests/unit/chart_series_ring_allocation.cpp`
(CTest `chart_series_ring`) prove:

- construction performs exactly one allocation and sets the requested capacity;
- after construction, appends (including five wrap-arounds past capacity), indexed reads, forward
  iteration, `spans()`, `oldest()`/`newest()`, `clear()` and a follow-up append allocate **zero
  blocks and zero bytes**, measured by replaced global `operator new`/`delete` counters (the
  technique of `tests/benchmarks/stylesheet/benchmark_counters.cpp`);
- logical order, wrap-around spans, oldest/newest, capacity 1, rejection of non-finite/equal/older
  timestamps without mutation, clear semantics and allocation-free moves;
- capacity 0 throws `std::invalid_argument`;
- the public operations that can be allocation-free are `noexcept` (compile-time asserts).

## 10. Non-goals

- Chart nodes, axes, scales, decimation, rendering and damage: #183.
- Multi-series aggregation, legend or tooltip state: component library work after #183.
- Persisting series data, cross-thread queues, or a new data transport: app/DataStore concerns.
- Resizable series storage: capacity is fixed. A resized chart constructs a new ring and copies the
  samples it still needs on the UI thread.
