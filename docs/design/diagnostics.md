# Diagnostics, tracing, and profiling

Status: design for Big Step B2/B3. Normative decision:
[ADR-022](../../decisions/ADR-022-diagnostics-and-profiling.md).

## 1. Goals

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

- RAII span macros:

  ```cpp
  TESSERA_TRACE_SCOPE("layout");
  TESSERA_TRACE_FRAME(frame_index);
  TESSERA_TRACE_TASK(task_id, "load_image");
  ```

- Spans nest and carry frame/tick/task/window ids.
- CPU spans: input, style, layout, paint, draw-list build, submit, present wait.
- GPU spans: timestamp queries per pass/pipeline; optional pipeline statistics.
- Async spans: task start/resume/await/cancel; queue wait vs run time.
- Export: Chrome Trace Event JSON / Perfetto protobuf; optional Tracy, ETW, ftrace, systrace,
  Instruments adapters.
- Sampling/flamegraph hooks where available; no symbols required for counters.

## 5. Metrics and counters

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

## 6. Debug overlay

- Toggleable dev overlay (`F1`/config), non-blocking, rendered through the normal 2D renderer.
- Shows: FPS/frame time, CPU/GPU time, draw calls/vertices, dirty counts, damage ratio, cache
  hit rate, async queue, memory, present/VRR, DPI, resize state, recent warnings.
- Never enabled by default in release unless explicitly configured.
- Overlay itself must not defeat dirty rendering (it marks only its own region dirty or runs in
  continuous debug mode).

## 7. Crash handling and breadcrumbs

- Platform crash handlers: SEH/minidump on Windows, signal handlers on Linux, Mach exception/
  signal on macOS, `ANR`/tombstone integration on Android later.
- Keep a bounded in-memory log ring and breadcrumbs (last frames/ticks, window metrics, GPU
  state, async tasks).
- Write a minidump/crash report atomically; include build id, GPU/driver, OS version, config
  (redacted).
- Symbolization via PDB/DWARF; optional upload is opt-in and redacted.
- Fatal path must not allocate unbounded memory or deadlock on the writer thread.

## 8. Frame capture and GPU debugging

- Trigger a GPU capture (RenderDoc/PIX/RGP) with a frame marker and a reproducible scene setup.
- Screenshot/frame dump: PNG + raw RGBA + structural report (node ids/bounds/styles).
- Frame graph/draw-list dump for diffing; golden diff integration (ADR-009).
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

Tooling/CI may force `tracing`/capture; release defaults are conservative.

## 10. Privacy and security

- No secrets, tokens, passwords, or full file contents in logs.
- Redact paths/user names by default where configured.
- Telemetry/upload is opt-in and documented; offline by default.
- Crash dumps may contain memory; treat as sensitive.
- Log files rotate and are size-capped; no unbounded growth.
- Security-relevant events use a dedicated category and never log payloads.

## 11. OS/tooling integration

- Windows: debugger output, ETW, PIX, WPA; minidump via DbgHelp/Crashpad.
- Linux: `stderr`, journald/file, perf/ftrace, RenderDoc, sysprof; core dumps.
- Wayland/X11: session logs; avoid blocking compositor calls in the log path.
- Android: logcat, systrace/Perfetto, tombstone (later).
- macOS: os_log/Instruments (later).
- Optional third-party: spdlog (logging), Tracy (profiling), Crashpad (crash). They must stay
  behind interfaces; no hard dependency for core rendering (see dependency map).

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

- Default production log level and retention.
- Built-in lightweight tracer vs Tracy/Perfetto adapters.
- GPU timestamp support on lavapipe/CI and fallback behavior.
- Crash upload service and privacy policy.
- Overlay rendering cost and its interaction with dirty/damage.
