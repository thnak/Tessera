# Diagnostics, tracing, and profiling

Status: the B2 logging/metrics core is implemented in `src/diagnostics/`
(`tessera_diagnostics`: `diagnostics/log.hpp`, `diagnostics/metrics.hpp`), together with bounded
tracing spans (`diagnostics/trace.hpp`) and an overlay line model (`diagnostics/overlay.hpp`);
tracing exporters, overlay rendering integration, crash reporting, capture, and file/console sinks
remain design for B2/B3. Big Steps are defined in
[AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step). Normative decision:
[ADR-022](../../decisions/ADR-022-diagnostics-and-profiling.md) (diagnostics and profiling). Crash-report pipeline:
[`crash-reporting-v1.md`](../specs/crash-reporting-v1.md); devtools:
[`devtools-protocol-v1.md`](../specs/devtools-protocol-v1.md).

## 1. Goals

Terms: tick = one fixed-step update of the runtime loop ([`game-loop.md`](game-loop.md)); damage = region
of the target that must be redrawn ([`dirty-rendering.md`](dirty-rendering.md)); RHI = render hardware
interface; VMA = Vulkan Memory Allocator (GPU memory library); VRR = variable refresh rate; lavapipe =
Mesa software Vulkan driver used for headless tests; golden = deterministic reference image (see
[`glossary.md`](../reference/glossary.md)); TTI = time to interactive; INP = interaction to next paint;
ETW = Event Tracing for Windows; PDB/DWARF = debug-symbol formats.

- Actionable diagnostics with bounded overhead, enabled in debug and optionally in production.
- Structured, correlatable data: logs, spans, metrics, counters, captures, crash reports.
- No hot-path allocations or blocking I/O; no secrets/PII by default.
- Deterministic tests do not depend on diagnostics; diagnostics never change rendering output.

## 2. Layers

| Layer | Question it answers | Overhead |
|---|---|---|
| Logging | what happened, with context | low, sampled |
| Tracing/spans | where did time go, in which frame/task | low, compile-time optional |
| Metrics | how much work/queue/frame time | very low |
| Profiling | which functions/GPU passes are hot | medium, explicit session |
| Overlay | live summary on screen | low, dev-only |
| Crash | what was the state at failure | on crash |
| Capture | reproduce/diff a frame | explicit trigger |

## 3. Structured logging

Implemented core (`diagnostics::Logger`, `diagnostics/log.hpp`):

- Levels `trace`..`fatal` and the category set from
  [`diagnostics-config-v1.md`](../specs/diagnostics-config-v1.md) are mirrored as enums; a
  category mask plus a minimum level filters records before the message is touched.
- `MemoryLogSink` is a bounded chronological ring for breadcrumbs/tests (`dropped()` counts
  discarded records). Sinks are non-owning and multiple sinks are supported.
- `redact_paths` replaces absolute Windows, POSIX, and macOS home prefixes with `<home>` and is on
  by default (`LogConfig::redact_paths`).
- `LogRecord` carries frame, tick, and window ids. Logging allocates only for an enabled record.
- Not implemented yet: file/console/debugger sinks, the async writer thread, rate limiting,
  sampling, category string config, and window-title redaction.

- Levels: `trace`, `debug`, `info`, `warn`, `error`, `fatal`.
- Categories: `app`, `window`, `input`, `style`, `layout`, `paint`, `rhi`, `vulkan`, `gpu`,
  `async`, `io`, `text`, `audio`, `security`.
- Fields: category, level, message, source file/line, thread/task id, frame/tick id,
  window/display id, key/value fields.
- Sinks: debugger, console, rotating file, in-memory ring (for crash breadcrumbs), optional
  ETW/perfetto/telemetry.
- Async-safe: lock-free per-thread ring buffers; a writer thread flushes; fatal path flushes
  synchronously.
- Bounded: ring size, file size, rotation, rate limiting per category/message.
- Redaction: paths, user names, tokens, passwords, window titles (configurable).
- Hot path: no `std::format`/allocation for disabled levels; compile-time category stripping in
  release if configured.

Example shape:

```json
{"t": 12.345, "lvl":"warn", "cat":"window", "msg":"swapchain out of date",
 "frame":42, "display":1, "size":[1440,900], "dpi":1.5, "reason":"resize"}
```

## 4. Tracing and spans

Implemented core (`diagnostics::TraceRecorder`, `diagnostics/trace.hpp`):

- A bounded ring of `Begin`/`End`/`Instant` events with category, nesting depth, frame, and tick.
  Timestamps are a deterministic sequence, not wall clock, so traces are reproducible.
- `TraceSpan` is RAII (records `End` once, movable) and needs no macros.
- `to_chrome_trace_json` emits Chrome Trace Event JSON with escaped names and frame/tick args.
- Compiled only with `TESSERA_ENABLE_TRACING=ON` (default `OFF`): a production build has no recorder,
  no event ring and no encoder, while the header keeps an empty no-op with the same signatures so
  call sites and the documented empty `trace` document survive
  ([`build-and-packaging.md`](build-and-packaging.md) section 2.1).
- Not implemented yet: wall-clock/GPU timestamps, trace sinks/rotation, sampling, Perfetto/ETW
  exporters, and automatic instrumentation.

`diagnostics::OverlayModel` (`diagnostics/overlay.hpp`) turns a frame snapshot plus metric samples
into short overlay lines; the batched2d proof draws them through `TextRenderer` with `--overlay`
and the `dashboard_overlay.png` lavapipe golden.

- RAII span macros:

  ```cpp
  TESSERA_TRACE_SCOPE("layout");
  TESSERA_TRACE_FRAME(frame_index);
  TESSERA_TRACE_TASK(task_id, "load_image");
  ```

- Spans nest and carry frame/tick/task/window ids, plus `uiId`/`componentId`/`nodeId` when the
  span is tied to a node (see [`automation-ids-v1.md`](../specs/automation-ids-v1.md)).
- CPU spans: input, style, layout, paint, draw-list build, submit, present wait;
  `element.attach/update/event/render/detach` for custom elements.
- GPU spans: timestamp queries per pass/pipeline; optional pipeline statistics.
- Async spans: task start/resume/await/cancel; queue wait vs run time.
- Export: Chrome Trace Event JSON / Perfetto protobuf; optional Tracy, ETW, ftrace, systrace,
  Instruments adapters.
- Sampling/flamegraph hooks on platforms whose profiler supports them (not specified per platform); no symbols required for counters.

## 5. Metrics and counters

Process memory: `diagnostics/memory.hpp` samples resident and peak bytes without allocating and
publishes them as gauges (budgets in [performance.md](performance.md) section 3.1).

Implemented core (`diagnostics::MetricsRegistry`, `diagnostics/metrics.hpp`):

- Fixed-capacity registry (`MetricsRegistry::kMaxMetrics`, default 128) of named counters and
  gauges. Names are allocated once at registration; increments are one relaxed atomic add, so
  frame/async hot paths do not allocate or block.
- `snapshot()` copies values in registration order for the overlay, tests, and future exporters;
  `reset()` zeroes values but keeps registrations. Registration past capacity returns
  `kInvalidMetric`, and operations on it are no-ops.
- Not implemented yet: histograms, sampling/rate limits, time-series retention, and exporters.

- Frame/tick: frame time, tick time, update/style/layout/paint/render/present wait, dropped
  steps, missed deadlines.
- Input: queue depth, coalesced events, hit tests, focus changes, input-to-style/render latency.
- Style/layout: dirty nodes, selector matches, computed-style cache hits/misses.
- Paint/render: draw calls, vertices, damage rects/ratio, full-redraw reason, cache hits/misses,
  bytes uploaded.
- GPU/memory: buffers/textures/descriptors, VMA stats, frames in flight, queue depth.
- Async: tasks created/completed/cancelled/failed, queue depth, run/wait time.
- Pacing/power: effective FPS, refresh, VRR, present mode, power policy, adaptive scale.
- Window: logical/physical size, DPI, safe area, focused/occluded/minimized, resize sequence.

Metrics are lock-free counters/gauges; snapshots are cheap and non-blocking.

### 5.1 Response-time and Lighthouse-style audits

- Response-time metrics: `input_to_handler`, `handler_duration`, `handler_to_paint`,
  `interaction_to_next_paint` (INP-like), startup/first-frame/TTI, route load, resource wait.
- Report p50/p95/p99 per interaction class (click/type/scroll) and long tasks/missed deadlines.
- Optional audit runner groups metrics into categories (responsiveness, startup, rendering,
  resources, reliability, accessibility), scores them 0..100, and emits JSON + trace.
- Raw metrics are primary; the score is a communication tool. No vendor GPU utilization score.
- Disabled by default; no frame-cost when off; no PII.
- Normative: [`lighthouse-audit-v1.md`](../specs/lighthouse-audit-v1.md); harness:
  [`headless-automation-v1.md`](../specs/headless-automation-v1.md).

## 6. Debug overlay

- Toggleable dev overlay (`F1`/config), non-blocking, rendered through the normal 2D renderer.
- Shows: FPS/frame time, CPU/GPU time, draw calls/vertices, dirty counts, damage ratio, cache
  hit rate, async queue, memory, present/VRR, DPI, resize state, recent warnings.
- Never enabled by default in release unless explicitly configured.
- Overlay itself must not defeat dirty rendering (it marks only its own region dirty or runs in
  continuous debug mode).

## 7. Crash handling and breadcrumbs

- Platform crash handlers: SEH/minidump on Windows, signal handlers on Linux, Mach exception/
  signal on macOS, `ANR`/tombstone integration on Android (in Big Step F, mobile; see [`mobile-integration.md`](mobile-integration.md)).
- Keep a bounded in-memory log ring and breadcrumbs (last frames/ticks, window metrics, GPU
  state, async tasks).
- Write a minidump/crash report atomically; include build id, GPU/driver, OS version, config
  (redacted).
- Symbolization via PDB/DWARF; optional upload is opt-in and redacted.
- Fatal path must not allocate unbounded memory or deadlock on the writer thread.

## 8. Frame capture and GPU debugging

- Trigger a GPU capture (RenderDoc/PIX/RGP) with a frame marker and a reproducible scene setup.
- Screenshot/frame dump: PNG + raw RGBA + structural report (node ids/bounds/styles).
- Frame graph/draw-list dump for diffing; golden diff integration ([ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md), deterministic headless golden tests).
- Validation: Vulkan validation layers in debug and CI; API validation; leak checks;
  GPU-assisted validation optional.
- Shader debug markers/pipeline names for vendor tools.

## 9. Configuration

`tessera.json` top-level `diagnostics` (see
[`diagnostics-config-v1.md`](../specs/diagnostics-config-v1.md)):

```json
{
  "diagnostics": {
    "level": "info",
    "categories": ["window", "rhi", "async"],
    "sinks": ["debugger", "file"],
    "file": "logs/tessera.log",
    "maxFileSizeMb": 32,
    "rotate": 3,
    "tracing": false,
    "gpuTimestamps": false,
    "overlay": false,
    "crashDumps": true,
    "redactPaths": true
  }
}
```

Tooling/CI may force `tracing`/capture; release defaults are the ones in [`diagnostics-config-v1.md`](../specs/diagnostics-config-v1.md).

## 10. Privacy and security

- No secrets, tokens, passwords, or full file contents in logs.
- Redact paths/user names when `redactPaths` is true (default `true` in [`diagnostics-config-v1.md`](../specs/diagnostics-config-v1.md)).
- Telemetry/upload is opt-in and documented; offline by default.
- Crash dumps may contain memory; treat as sensitive.
- Log files rotate and are size-capped; no unbounded growth.
- Security-relevant events use a dedicated category and never log payloads.

## 11. OS/tooling integration

- Windows: debugger output, ETW, PIX, WPA; minidump via DbgHelp/Crashpad.
- Linux: `stderr`, journald/file, perf/ftrace, RenderDoc, sysprof; core dumps.
- Wayland/X11: session logs; avoid blocking compositor calls in the log path.
- Android: logcat, systrace/Perfetto, tombstone (Big Step F, mobile).
- macOS: os_log/Instruments (macOS is deferred, [ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md)).
- Optional third-party: spdlog (logging), Tracy (profiling), Crashpad (crash). They must stay
  behind interfaces; no hard dependency for core rendering (see [`dependencies.md`](../specs/dependencies.md)).

## 12. Performance rules

- Disabled logs/traces compile to a branch or no-op; no formatting/allocations.
- Per-thread ring buffers; a background writer flushes; fatal path is synchronous.
- Sampling: trace/metrics can be sampled per frame/task to bound overhead.
- No `std::string` in the per-draw/per-node hot path; use interned ids/categories.
- Diagnostics must not create frames or wake the loop; overlay does, by design.

## 13. Tests

- Log schema/level/category filtering; redaction.
- Trace export validity (Chrome/Perfetto schema) and span nesting.
- Metric counters under synthetic load; no overflow/underflow.
- Crash handler smoke test in a child process.
- Frame capture reproducibility in headless mode.
- Blocking detector: no blocking waits on the UI thread.
- Diagnostics disabled: zero output and no measurable frame-time change.

## 14. Open questions

- Default production log level and retention (no decision yet).
- Built-in lightweight tracer vs Tracy/Perfetto adapters.
- GPU timestamp support on lavapipe/CI and fallback behavior.
- Crash upload service and privacy policy.
- Overlay rendering cost and its interaction with dirty/damage.
