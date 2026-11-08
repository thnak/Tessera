# Diagnostics and profiling configuration v1

Status: proposed for Big Steps B2 and B3 (defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Extends [`project-config-v1.md`](project-config-v1.md)
with the top-level `diagnostics` object. Design: [`diagnostics.md`](../design/diagnostics.md),
decision: [ADR-022](../../decisions/ADR-022-diagnostics-and-profiling.md) (diagnostics and profiling).

## 1. Schema

```json
{
  "diagnostics": {
    "level": "info",
    "categories": ["*"],
    "sinks": ["debugger", "ring"],
    "file": "logs/tessera.log",
    "maxFileSizeMb": 32,
    "rotate": 3,
    "ringEntries": 4096,
    "sampleRate": 1.0,
    "tracing": false,
    "tracingSink": "file",
    "tracingFile": "logs/tessera.trace",
    "profilingBackend": "none",
    "gpuTimestamps": false,
    "overlay": false,
    "overlaySections": ["frame", "gpu", "dirty", "async", "memory"],
    "validation": "auto",
    "frameCapture": "manual",
    "crashDumps": true,
    "crashDumpDir": "logs/crash",
    "breadcrumbs": 256,
    "blockingWaitDetection": true,
    "redactPaths": true,
    "redactWindowTitles": false,
    "telemetry": false,
    "audit": {
      "enabled": false,
      "profile": "desktop",
      "reportPath": "artifacts/audit.json",
      "failOnScoreBelow": 0,
      "thresholds": { "interactionP95Ms": 100, "startupMs": 1000 }
    }
  }
}
```

## 2. Fields

Sink and backend names are OS or tool names: ETW (Event Tracing for Windows), Perfetto and Tracy (tracing/profiling tools), Instruments (Apple's profiler); see section 7 and [`dependencies.md`](dependencies.md) section 8.

| Field | Type | Default | Values | Notes |
|---|---|---|---|---|
| `level` | string | `info` | `trace`, `debug`, `info`, `warn`, `error`, `fatal` | Minimum log level. |
| `categories` | string[] | `["*"]` | category names or `*` | Enabled log categories. |
| `sinks` | string[] | `["debugger","ring"]` | `debugger`, `console`, `file`, `ring`, `etw`, `perfetto` | Multiple sinks allowed. |
| `file` | string | `logs/tessera.log` | relative path | Rotating file sink path. |
| `maxFileSizeMb` | integer | `32` | `1..4096` | Per-file cap. |
| `rotate` | integer | `3` | `0..64` | Rotated files retained; `0` disables rotation. |
| `ringEntries` | integer | `4096` | `64..1048576` | In-memory ring for overlay/crash breadcrumbs. |
| `sampleRate` | number | `1.0` | `0.0..1.0` | Trace/metrics sampling (1 = all). |
| `tracing` | boolean | `false` | | Enable span tracing. |
| `tracingSink` | string | `file` | `file`, `perfetto`, `etw`, `tracy` | Trace exporter. |
| `tracingFile` | string | `logs/tessera.trace` | relative path | Used by the file sink. |
| `profilingBackend` | string | `none` | `none`, `tracy`, `perfetto`, `etw`, `instruments` | Optional build-time backend. |
| `gpuTimestamps` | boolean | `false` | | GPU timestamp queries if supported. |
| `overlay` | boolean | `false` | | Dev overlay; never default in release. |
| `overlaySections` | string[] | frame/gpu/dirty/async/memory | section names | Overlay content. |
| `validation` | string | `auto` | `auto`, `on`, `off` | Vulkan/API validation; `auto` = on in debug. |
| `frameCapture` | string | `manual` | `manual`, `on-error`, `off` | GPU capture trigger policy. |
| `crashDumps` | boolean | `true` | | Enable crash handler/dumps. |
| `crashDumpDir` | string | `logs/crash` | relative path | Dump/core output. |
| `breadcrumbs` | integer | `256` | `0..8192` | Ring entries included in crash reports (breadcrumbs, [`crash-reporting-v1.md`](crash-reporting-v1.md)). |
| `blockingWaitDetection` | boolean | `true` | | Debug assert on UI-thread blocking waits. |
| `redactPaths` | boolean | `true` | | Redact user paths in logs/dumps. |
| `redactWindowTitles` | boolean | `false` | | Redact window titles. |
| `telemetry` | boolean | `false` | | Opt-in upload; offline by default. |
| `audit.enabled` | boolean | `false` | | Enable Lighthouse-style audit collection ([`lighthouse-audit-v1.md`](lighthouse-audit-v1.md)). |
| `audit.profile` | string | `desktop` | `desktop`, `mobile`, `power-save`, `custom` | Audit profile. |
| `audit.reportPath` | string | `artifacts/audit.json` | relative path | JSON report output. |
| `audit.failOnScoreBelow` | number | `0` | `0..100` | Optional CI failure threshold. |
| `audit.thresholds` | object | `{}` | metric -> number | Raw metric thresholds. |

## 3. Validation rules

1. `level` and `categories` must be known; unknown categories are errors.
2. `sinks`, `tracingSink`, `profilingBackend`, `validation`, and `frameCapture` must be known
   values.
3. `file`, `tracingFile`, and `crashDumpDir` must be relative and inside the project/output
   directory; absolute paths are errors.
4. `maxFileSizeMb`, `rotate`, `ringEntries`, `breadcrumbs`, and `sampleRate` must be in range.
5. `overlay=true` is allowed in debug; release packaging warns or rejects (which of the two is not specified) unless explicitly allowed.
6. `telemetry=true` requires an explicit privacy acknowledgement; default is false.
7. `gpuTimestamps`/`profilingBackend` fall back silently if the backend/device/tool is
   unavailable.
8. `frameCapture=on-error` only triggers on crash/device-loss, never in normal frames.
9. `blockingWaitDetection` is debug-only; release builds ignore it with a diagnostic.
10. Diagnostics must not change rendering output or golden results (reference images, [`golden-tests-v1.md`](golden-tests-v1.md)).
11. `audit.reportPath` is relative and inside the output/artifacts directory; audit is disabled
    by default and thresholds are non-negative.
12. Audit metrics/scores follow [lighthouse-audit-v1.md](lighthouse-audit-v1.md); raw metrics are
    primary, and disabling audit must add no measurable frame cost.

## 4. C++ API mirror

```cpp
namespace tessera {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };
enum class ProfilingBackend { None, Tracy, Perfetto, Etw, Instruments };
enum class ValidationMode { Auto, On, Off };
enum class FrameCaptureMode { Manual, OnError, Off };

struct DiagnosticsConfig {
    LogLevel level = LogLevel::Info;
    std::vector<std::string> categories{"*"};
    std::vector<std::string> sinks{"debugger", "ring"};
    std::string file = "logs/tessera.log";
    std::uint32_t max_file_size_mb = 32;
    std::uint32_t rotate = 3;
    std::uint32_t ring_entries = 4096;
    float sample_rate = 1.0f;
    bool tracing = false;
    std::string tracing_sink = "file";
    std::string tracing_file = "logs/tessera.trace";
    ProfilingBackend profiling_backend = ProfilingBackend::None;
    bool gpu_timestamps = false;
    bool overlay = false;
    std::vector<std::string> overlay_sections{"frame", "gpu", "dirty", "async", "memory"};
    ValidationMode validation = ValidationMode::Auto;
    FrameCaptureMode frame_capture = FrameCaptureMode::Manual;
    bool crash_dumps = true;
    std::string crash_dump_dir = "logs/crash";
    std::uint32_t breadcrumbs = 256;
    bool blocking_wait_detection = true;
    bool redact_paths = true;
    bool redact_window_titles = false;
    bool telemetry = false;
};

}
```

## 5. Profiles

### Development

```json
{ "diagnostics": { "level": "debug", "sinks": ["debugger","file"], "tracing": true,
  "overlay": true, "validation": "on", "blockingWaitDetection": true } }
```

### CI / headless

```json
{ "diagnostics": { "level": "info", "sinks": ["file"], "tracing": false,
  "gpuTimestamps": true, "validation": "on", "crashDumps": false, "frameCapture": "off" } }
```

### Production

```json
{ "diagnostics": { "level": "warn", "sinks": ["file","ring"], "tracing": false,
  "overlay": false, "validation": "auto", "crashDumps": true, "telemetry": false,
  "redactPaths": true } }
```

## 6. Privacy and security

- No secrets/payloads in logs or dumps; redact paths/user names by default.
- Telemetry/upload is opt-in and documented; offline by default.
- Crash dumps may contain memory; treat them as sensitive.
- Rotate and size-cap files; no unbounded growth.
- Security events use a dedicated category and never log payloads.

## 7. OS/tool integration

- Windows: debugger output, ETW, PIX/WPA, minidumps (DbgHelp/Crashpad).
- Linux: stderr/journald/file, perf/ftrace, RenderDoc, sysprof, core dumps.
- Android: logcat, Perfetto/systrace, tombstones (Android: Big Step F).
- macOS: os_log/Instruments (macOS: deferred, [ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md) (macOS via MoltenVK, deferred)).
- Optional dependencies (spdlog/Tracy/Crashpad, see [`dependencies.md`](dependencies.md) section 8) stay behind interfaces; core rendering does not
  require them.

## 8. Open questions

- Default production log level/retention.
- Built-in tracer vs Perfetto/Tracy adapters.
- Crash upload service/privacy policy.
- Overlay cost and dirty/damage interaction.
- GPU timestamp support on lavapipe (Mesa's software Vulkan driver used in CI) and CI.
