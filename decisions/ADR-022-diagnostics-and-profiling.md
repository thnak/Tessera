# ADR-022: Structured diagnostics, tracing, and profiling

- Status: Proposed
- Date: 2026-11-07
- Related: ADR-009, ADR-018, ADR-019, ADR-021

## Context

A deterministic, headless-friendly UI framework needs first-class diagnostics: AI agents and
humans must see why a frame was slow, why a resize stalled, which async task blocked, and what
changed visually. Ad-hoc `printf` logging is not enough, and always-on heavyweight profilers are
not acceptable in production.

## Decision

1. Provide layered diagnostics: structured logging, tracing/spans, metrics/counters, debug
   overlay, profiling hooks, crash reports, and frame capture.
2. **Structured logging** with levels/categories/fields/source/thread/frame ids; bounded async
   sinks (debugger, rotating file, in-memory ring, optional ETW/Perfetto); redaction; no hot-path
   allocation/formatting when disabled.
3. **Tracing** with RAII spans for frame/tick/input/style/layout/paint/render/async/GPU; export
   Chrome Trace/Perfetto; optional Tracy/ETW/ftrace/systrace/Instruments adapters.
4. **Metrics**: frame/CPU/GPU time, present wait, steps/dropped, input latency, dirty counts,
   draw calls/vertices, damage ratio, cache hits, uploads, queue depth, memory, present/VRR,
   DPI/resize state, power/adaptive quality.
5. **Debug overlay** is dev-only, non-blocking, and must not defeat dirty rendering.
6. **Crash handling**: bounded breadcrumb ring, minidump/core dump, build/GPU/OS/config metadata
   (redacted), symbolization; upload is opt-in.
7. **Frame capture**: GPU capture markers, screenshot/raw RGBA + structural report, draw-list/frame
   graph dump, golden diff integration.
8. **Validation**: Vulkan validation layers in debug/CI, API validation, leak checks; optional
   GPU-assisted validation.
9. Optional third-party implementations (spdlog/Tracy/Crashpad) stay behind interfaces; core
   rendering must not require them.
10. Privacy/security: no secrets/payloads, redact paths/user names, bounded rotating files,
    opt-in telemetry, treat dumps as sensitive.
11. Diagnostics must not change rendering output or deterministic goldens; disabled paths add zero
    measurable frame time.
12. Configuration lives in `tessera.json.diagnostics`; CI/tools may force tracing/capture.

## Consequences

### Positive

- Fast root-cause analysis for frame spikes, resize stalls, async blocking, GPU errors.
- AI-agent feedback includes structured layout/style/metrics plus visual diffs.
- Production-safe logging and opt-in deep profiling.
- Clear privacy defaults.

### Negative / risks

- Tracing/profiling overhead if misconfigured.
- Platform-specific crash/tracing integrations add maintenance.
- Log/dump privacy requires discipline and tests.
- Overlay can interact with dirty/damage and frame pacing.

## Alternatives considered

- **printf/console logging only.** Rejected: no structure, correlation, or metrics.
- **Always-on Tracy/Perfetto.** Rejected: overhead, build size, not production-default.
- **No crash reporting.** Rejected: unacceptable for shipped apps.
- **Telemetry by default.** Rejected: privacy and trust.
- **Diagnostics coupled to the renderer.** Rejected: must work in headless/software paths too.

## Follow-up

- Implement the structured logger, ring buffer, and rotating file sink.
- Implement trace spans/export and the debug overlay.
- Integrate Vulkan timestamps/validation and capture markers.
- Add crash dumps/breadcrumbs and redaction tests.
- Add a diagnostics config spec and wire it into `tessera.json`.
