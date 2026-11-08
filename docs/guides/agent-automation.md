# Coding-agent automation guide

Status: proposed (B2/C). Companion to
[`automation-ids-v1.md`](../specs/automation-ids-v1.md),
[`headless-automation-v1.md`](../specs/headless-automation-v1.md), and
[`lighthouse-audit-v1.md`](../specs/lighthouse-audit-v1.md).

This is how an agent finds, drives, traces, and audits the UI without brittle coordinates.

## 1. Build with automation IDs

- The compiler emits `generated/automation.manifest.json`.
- C++ has `UiId`, `ComponentId`, `NodeId` as `uint64_t`.
- JSON uses decimal strings (uint64 precision).
- Add explicit `automationId="1234567890123456789"` to long-lived targets whose source path may
  move.

Manifest gives you `uiId -> component/kind/source path`. Read it instead of guessing.

## 2. Launch headless

```bash
tessera test --script tests/agent/home.json --report out/report.json
```

Modes:

- `structural` (null backend) — fast layout/style/paint/events, no pixels.
- `golden` (software/lavapipe) — pixels + report.
- `gpu-smoke` — backend smoke only; never updates goldens.
- `audit` — Lighthouse-style metrics/score.

## 3. Find nodes

Prefer stable IDs:

```json
{"action":"waitFor","uiId":"987654321098765432","state":"visible","timeoutMs":1000}
{"action":"click","uiId":"222222222222222222"}
{"action":"assert","uiId":"333333333333333333","text":"Welcome, alice"}
```

Queries:

- `uiId` — stable node/component target.
- `(uiId,itemKey)` — list/grid item instance.
- `componentId` — all nodes from a component.
- `role` + accessible `name` — semantic fallback.
- `nodeId` — runtime instance for debugging/trace only; not stable across runs.

Do not use screen coordinates as identity. Coordinates are only for visual debugging.

## 4. Snapshot

```json
{"action":"snapshot","path":"out/snapshot.json"}
```

Snapshot contains `nodeId`, `uiId`, `componentId`, parent/children, role/name/text/value,
bounds, visible/enabled/focused/selected, and `itemKey`. It does not expose raw pointers.

## 5. Actions

`click`, `type`, `key`, `scroll`, `focus`, `resize` go through the normal input/runtime path.
They do not mutate state directly. `waitFor` observes frame boundaries with a timeout.

## 6. Trace

```json
{"action":"trace","path":"out/trace.json"}
```

Spans carry `traceId`, `spanId`, `uiId`, `componentId`, `nodeId`, and names like
`event.click-handler`, `style`, `layout`, `paint`, `present`. Use them to answer "why was this
interaction slow?" and "which component caused this frame?".

## 7. Audit

```json
{"action":"audit","profile":"desktop","path":"out/audit.json"}
```

Metrics include startup/first frame/TTI, `input_to_handler`, `handler_to_paint`,
`interaction_to_next_paint` p50/p95/p99, frame time/jank, resource wait/cache, errors/boundary
catches, memory, idle presents. The report has raw metrics + category/overall scores.

Use raw metrics for decisions; don't chase the score.

## 8. Example session

```json
{
  "scriptVersion": 1,
  "profile": {"width":1280,"height":800,"scale":1.0,"locale":"en-US"},
  "steps": [
    {"action":"launch"},
    {"action":"waitFor","uiId":"111111111111111111","state":"visible","timeoutMs":1500},
    {"action":"type","uiId":"111111111111111111","text":"alice"},
    {"action":"click","uiId":"222222222222222222"},
    {"action":"assert","uiId":"333333333333333333","text":"Welcome, alice"},
    {"action":"audit","profile":"desktop","path":"out/audit.json"},
    {"action":"trace","path":"out/trace.json"}
  ]
}
```

### 8.1 Custom elements

- Target by `uiId`; sub-targets use `partId` (3D picking).
- Use `custom`/`assertCustom` for element test commands and snapshot paths.
- Snapshot includes element id, state, draw stats, and last event.
- Structural mode records draws without GPU; golden uses compiled shaders.
- See [`custom-elements-v1.md`](../specs/custom-elements-v1.md).

## 9. Rules for agents

- Use manifest IDs; don't scrape string paths.
- Prefer `uiId`/`(uiId,itemKey)` over coordinates.
- `nodeId` is runtime-only; never persist it.
- Wait for state, don't sleep.
- Assert semantics, not just pixels.
- Keep scripts deterministic; no network/system fonts/wall clock.
- On failure, read `report.json` + snapshot + trace before changing code.
- Do not update goldens automatically; golden updates are reviewed.

## 10. Troubleshooting

| Symptom | Check |
|---|---|
| ID not found | manifest version/build; node not realized (virtualized/offscreen) |
| Wrong item in list | use `(uiId,itemKey)`, not `uiId` alone |
| ID changed | source moved/renamed; use explicit `automationId` |
| Timeout | wait for state; check async/resource load/error boundary |
| Pixel diff but structure same | text/AA/DPI/font; golden mode/tolerance |
| Audit noisy | profile/machine mismatch; compare same profile/baseline |

## 11. Status

- Specs only; harness/manifest not implemented.
- IDs are build-time; trace/audit are opt-in and redacted.
