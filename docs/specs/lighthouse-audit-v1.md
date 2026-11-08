# Lighthouse-style runtime audits v1

Status: proposed (Big Steps B2 and F, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step), section "Big
Steps"). Decision:
[ADR-027](../../decisions/ADR-027-agent-automation-and-audits.md) (numeric automation IDs,
headless harness, Lighthouse-style audits). Companion to
[`diagnostics.md`](../design/diagnostics.md), [`startup-and-loading.md`](../design/startup-and-loading.md),
and [`headless-automation-v1.md`](headless-automation-v1.md).

A native-app audit, not a web Lighthouse clone: measure startup, responsiveness, rendering,
resources, reliability, and accessibility, report raw metrics + a scored report, and gate CI.

## 1. Principles

1. **Measure user-visible latency**, not vendor GPU utilization.
2. **Report raw metrics and score**; never optimize only the score.
3. **Deterministic scenes/profiles**; repeated runs are comparable.
4. **Opt-in**; zero measurable overhead when disabled.
5. **No PII/secrets**; redact traces/reports.
6. **Thresholds + baseline**; regressions fail CI.

## 2. Profiles

| Profile | Assumptions |
|---|---|
| `desktop` | 1280×800, 1.0 scale, 60 Hz, local assets |
| `mobile` | 390×844, 3.0 scale, 60/120 Hz, cold start, constrained memory |
| `power-save` | reduced FPS/quality, no prefetch |
| `custom` | explicit size/scale/refresh/network/memory |

Each profile pins viewport/DPI/fonts/locale/state. Cold vs warm start is explicit. Network is
local/offline unless the audit includes services.

## 3. Metrics

### Startup

- cold/warm start to first frame
- time to first paint (TTFP) / first meaningful screen
- time to interactive (TTI)
- auth/session check time
- route/shell load time

### Responsiveness

- `input_to_handler` — input timestamp to handler start
- `handler_duration`
- `handler_to_paint` — handler end to frame present
- `interaction_to_next_paint` (INP-like; INP is the web "Interaction to Next Paint" metric) = input timestamp → next presented frame
- p50/p95/p99 per interaction class (click/type/scroll)
- long tasks (> budget) and missed input deadlines

### Rendering

- frame time (CPU/GPU if available), missed frame deadline, jank
- draw calls, vertices, batches, overdraw proxy (optional)
- present mode/refresh/VRR (variable refresh rate) decisions
- static-idle presents/submissions (should be 0)

### Resources

- load/wait time, cache hit/miss, bytes, preload hit/miss, unload/thrash
- GPU/cache memory peak

### Reliability

- uncaught errors, error-boundary catches, crashes, timeouts, queue overflow
- route-load failures, auth failures, offline fallbacks

### Accessibility

- interactive nodes without names/roles
- focus order/visibility, hit-target size (where measurable)
- text scale/high contrast/reduced-motion checks

### Memory/Power

- peak memory by category, cache growth
- idle/background GPU work, frame budget, thermal/battery policy

## 4. Audits and scoring

Each audit:

```json
{
  "id": "interaction-latency",
  "title": "Interaction latency",
  "category": "responsiveness",
  "metric": "interaction_to_next_paint.p95",
  "value": 92.4,
  "unit": "ms",
  "direction": "lower-is-better",
  "good": 50,
  "poor": 200,
  "weight": 1.0,
  "score": 72,
  "status": "warn",
  "advice": "Reduce handler work and avoid layout/paint in the input path."
}
```

Score mapping (clamped 0..100):

```text
lower-is-better:  score = 100 * (poor - value) / (poor - good)
higher-is-better: score = 100 * (value - poor) / (good - poor)
```

- `value <= good` -> 100; `value >= poor` -> 0 (reverse for higher-is-better).
- Status: `pass` (>=90), `warn` (50..89), `fail` (<50) or threshold config.
- Category score = weighted average of its audits.
- Overall score = weighted average of categories.

Suggested category weights (starting values; section 10 lists exact weights per profile as an
open question):

| Category | Weight |
|---|---|
| Responsiveness | 30 |
| Startup | 20 |
| Rendering | 20 |
| Resources | 15 |
| Reliability | 10 |
| Accessibility | 5 |

Weights are profile-configurable; document changes. Raw metrics are always reported.

## 5. Report

```json
{
  "auditVersion": 1,
  "profile": "desktop",
  "run": { "cold": true, "commit": "...", "configHash": "..." },
  "score": 84,
  "categories": [
    { "id": "responsiveness", "score": 78, "weight": 30 }
  ],
  "metrics": {
    "startupMs": 412,
    "ttfpMs": 180,
    "ttiMs": 620,
    "interactionToNextPaintP95Ms": 92.4,
    "frameTimeP95Ms": 14.2,
    "resourceWaitP95Ms": 35.0,
    "boundaryCatches": 0,
    "idlePresents": 0
  },
  "audits": [],
  "artifacts": { "trace": "trace.json", "screenshots": ["frame.png"] }
}
```

- JSON is canonical; HTML is a rendering of it.
- Reports include enough context (profile/commit/config hash) to compare runs.
- No secrets/PII; traces follow diagnostics redaction.

## 6. Measurement

- Monotonic clock; input timestamps captured at the platform boundary.
- Span chain: input → handler → style → layout → paint → present.
- `interaction_to_next_paint` ends at the first presented frame after the input.
- p50/p95/p99 computed over the run; interaction classes separated.
- Frame time and missed deadlines use the same clock as the loop.
- GPU timestamps optional; if unsupported, use CPU frame budget and work counters.
- Sampling/overhead budget; disabled by default in release.
- Deterministic harness for scene/input; latency uses real elapsed time in audit mode.

## 7. CI and baselines

- Run fixed audit profiles on a fixed machine class (CPU/GPU variance documented; the machine
  class is not specified).
- Fail on threshold regression or score drop beyond tolerance.
- Compare raw metrics to baseline; do not compare across different profiles.
- Upload report/trace/screenshots; keep synthetic data only.
- Nightly/full audit; PR fast structural audit (structural mode: see
  [`headless-automation-v1.md`](headless-automation-v1.md) section 1).

## 8. Cautions

- Score is a communication tool, not a target; optimize raw latency/memory.
- Vendor GPU utilization is not a portable metric; don't score it.
- Cold/warm/mobile/power profiles are not comparable.
- Audits must not change rendering output or golden pixels.
- Disabled audit code must add no measurable frame cost.

## 9. Tests

- Metric math (input/handler/paint/present; percentiles).
- Score interpolation/clamping/weights; category/overall.
- Deterministic harness/repeated runs.
- Trace correlation (`uiId`/`componentId`/`nodeId`/span names).
- Threshold/baseline failure; report schema; redaction.
- Disabled path overhead; no pixel changes.

## 10. Open questions

- Exact thresholds/weights per app profile.
- Real-vs-simulated time in CI.
- GPU timestamp support on lavapipe (Mesa software Vulkan driver).
- Accessibility audit depth.
- Baseline storage/trending and flake policy.
- HTML report generation.
