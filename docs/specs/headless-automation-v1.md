# Headless automation v1

Status: proposed (Big Step B2/C). Decision: ADR-027. Companion to
[`testing.md`](../design/testing.md), [`golden-tests-v1.md`](golden-tests-v1.md), and
[`automation-ids-v1.md`](automation-ids-v1.md).

A deterministic headless harness lets coding agents and CI drive the real UI without a window,
GPU, OS event loop, or network.

## 1. Modes

| Mode | Backend | Output | Use |
|---|---|---|---|
| `structural` | null | snapshot, layout/style/paint stats, events | fast agent tests |
| `golden` | software/lavapipe | snapshot + PNG + report | visual regression |
| `gpu-smoke` | Vulkan | frames/metrics, tolerance | backend smoke |
| `audit` | software or GPU | Lighthouse-style audit JSON | performance/quality gates |

Goldens always come from the software path (ADR-009). `gpu-smoke` never updates baselines.

## 2. Harness

- Fake HAL: window/display/input/power/lifecycle/timer; no real OS callbacks.
- Fixed size, DPI/scale, fonts, locale, time zone, and asset set.
- Simulated monotonic clock and fixed timestep.
- Deterministic executor; no detached tasks/network/system fonts.
- Real UI/style/layout/paint/renderer code; only the platform/GPU edges are fake/software.
- App config loaded from `tessera.json`; automation manifest loaded from `generated/`.

## 3. CLI

```bash
tessera test --script tests/agent/login.json --report out/report.json
tessera test --script tests/agent/scroll.json --mode golden --out out/
tessera headless --mode audit --profile desktop --script tests/audit/home.json
```

Exit codes: `0` pass, `1` assertion/audit failure, `2` invalid script/config.

## 4. Script schema

```json
{
  "scriptVersion": 1,
  "profile": { "width": 1280, "height": 800, "scale": 1.0, "locale": "en-US" },
  "app": { "config": "tessera.json" },
  "steps": [
    { "action": "launch" },
    { "action": "waitFor", "uiId": "987654321098765432", "state": "visible", "timeoutMs": 1000 },
    { "action": "type", "uiId": "111111111111111111", "text": "alice" },
    { "action": "click", "uiId": "222222222222222222" },
    { "action": "assert", "uiId": "333333333333333333", "text": "Welcome, alice" },
    { "action": "snapshot", "path": "out/after-login.json" },
    { "action": "screenshot", "path": "out/after-login.png" },
    { "action": "trace", "path": "out/trace.json" },
    { "action": "audit", "profile": "desktop", "path": "out/audit.json" }
  ]
}
```

Actions: `launch`, `close`, `resize`, `click`, `type`, `key`, `scroll`, `focus`, `waitFor`,
`assert`, `snapshot`, `screenshot`, `trace`, `audit`, `custom`, `assertCustom`, `navigate`,
`back`, `openOverlay`, `closeOverlay`, `assertRoute`, `assertText`, `assertSelection`.

Assertions:

- node exists/visible/enabled/focused/selected
- text/value/bounds/role/name
- list item exists `(uiId, itemKey)`
- event count/order
- no error-boundary catch/crash
- frame/metrics budgets
- golden image match
- audit score/threshold
- custom element state/draw stats/last event (`custom`/`assertCustom`)
- route/params, text/selection, overlay open/close/focus

### 4.1 Custom elements

- Target a custom element by `uiId`; sub-targets may use `partId` from `hit_test_part`.
- `custom` invokes a debug/headless-only `on_test_command`; `assertCustom` reads
  `CustomElementSnapshot` paths.
- Structural mode records passes/pipelines/draws without a GPU; golden mode renders offscreen
  with compiled shaders; GPU smoke validates a real device.
- Snapshots include element id, state, draw counts/pipeline names, metrics, and last event.
- `continuous` elements advance by the fixed frame script, never wall clock.
- See [`custom-elements-v1.md`](custom-elements-v1.md).

## 5. Outputs

```text
out/
├── report.json        # pass/fail, steps, metrics, diagnostics
├── snapshot.json      # UiSnapshot dump
├── frame.png          # screenshot (golden/structural optional)
├── trace.json         # Chrome/Perfetto-compatible spans
└── audit.json         # Lighthouse-style audit (optional)
```

`report.json` includes step index, action, target ID, result, duration, error, and artifact
paths. Failures include a diagnostic and, where possible, a minimal repro script.

## 6. Agent protocol

For coding agents, the harness also exposes a line-delimited JSON protocol over stdin/stdout
(or a local socket):

```json
{"id":1,"cmd":"launch","script":"tests/agent/home.json"}
{"id":2,"cmd":"snapshot"}
{"id":3,"cmd":"click","uiId":"222222222222222222"}
{"id":4,"cmd":"waitFor","uiId":"333333333333333333","state":"visible","timeoutMs":1000}
{"id":5,"cmd":"trace","path":"out/trace.json"}
{"id":6,"cmd":"audit","profile":"desktop"}
```

- Responses echo `id`, `ok`, `result`/`error`, and timing.
- Commands are serialized; no concurrent UI mutation.
- IDs are decimal strings (uint64).
- The protocol is local-only and authenticated by the harness process; no network exposure.
- A command timeout aborts the step and returns a diagnostic; the harness remains usable.

## 7. Determinism

- Fixed clock/timestep, size/DPI/fonts/locale, assets, and event scripts.
- No wall clock, network, system fonts, or random IDs in structural/golden modes.
- Snapshot order stable; manifest IDs stable.
- Async completions applied at frame boundaries in deterministic order.
- Audit mode may use real elapsed time for latency but runs a fixed scene/script; it does not
  affect golden pixels.

## 8. CI

- Run structural tests on every PR (fast, no GPU).
- Run golden tests on lavapipe/software (ADR-009).
- Run GPU smoke where available; never update goldens from it.
- Run audits on a fixed profile with thresholds; upload report/trace/screenshots.
- Fail on assertion, crash, error-boundary catch, audit threshold, or golden diff.

## 9. Security and privacy

- Harness is local-only; no arbitrary filesystem/network access from scripts.
- Screenshots/traces may contain user data; fixtures must be synthetic.
- Redact secrets/PII; no telemetry by default.
- Scripts are untrusted input: validate paths/actions/IDs; reject traversal/oversized payloads.

## 10. Tests of the harness

- Script parsing/validation and unknown actions.
- Deterministic repeated runs (same manifest/snapshot/pixels).
- Query/action through the real input path.
- Custom element unit/structural/golden/GPU-smoke paths; custom commands/asserts; part ids.
- Timeout/waitFor cancellation; harness remains usable.
- Report/trace/snapshot schema; uint64 as strings.
- Golden/GPU-smoke separation; no baseline updates from GPU.
- No network/system-font access; path traversal rejected.

## 11. Open questions

- Protocol transport (stdin/stdout vs local socket) and auth.
- Snapshot size/compression for large trees.
- Parallel test isolation (multiple harness processes).
- Whether audit runs use real time or simulated time for latency.
- Headless accessibility tree exposure.
