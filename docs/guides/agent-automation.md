# Coding-agent automation guide

Status: proposed (Big Steps B2 and C; see [AGENTS.md, Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Decision: [ADR-027](../../decisions/ADR-027-agent-automation-and-audits.md) (agent automation and audits). Companion to
[`automation-ids-v1.md`](../specs/automation-ids-v1.md) (numeric UI ids),
[`headless-automation-v1.md`](../specs/headless-automation-v1.md) (headless scripts and reports), and
[`lighthouse-audit-v1.md`](../specs/lighthouse-audit-v1.md) (Lighthouse-style audit metrics).

This is how an agent finds, drives, traces, and audits the UI without brittle coordinates.

## 1. Build with automation IDs

- The compiler emits `generated/automation.manifest.json`.
- C++ has `UiId` (build-time stable id of a UI node/component), `ComponentId`, and `NodeId` (runtime instance id, not stable across runs) as `uint64_t`.
- JSON uses decimal strings (uint64 precision).
- Add explicit `automationId="1234567890123456789"` to long-lived targets whose source path may
  move (the id would otherwise change with the path).

Manifest gives you `uiId -> component/kind/source path`. Read it instead of guessing.

To drive your app instead of the built-in fixture, put `"app": { "config": "tessera.json" }` in
the script (path relative to the working directory). `tessera test --script` then loads that
`tessera.json` and `generated/automation.manifest.json` (or `codegen.outDir`), builds a scene with
one node per manifest node, and resolves the script's `uiId`s against it. The scene is structural
(kinds, tree, focus, typing, scrolling); app event handlers do not run. A missing, oversized or
invalid config/manifest exits `2`.

## 2. Launch headless

```bash
tessera test --script tests/agent/home.json --report out/report.json
```

Modes:

- `structural` (null RHI backend that records commands without a GPU) — fast layout/style/paint/events, no pixels.
- `golden` (software path: Mesa lavapipe, a software Vulkan driver, or the CPU rasterizer) — pixels + report; a golden is a deterministic reference image.
- `gpu-smoke` — backend smoke only; never updates goldens.
- `audit` — Lighthouse-style metrics/score (see section 7).

Without a native toolchain, the `tessera-mcp` stdio mode runs the same engine compiled to Wasm in
Node and exposes these commands as MCP tools (compile-only first)
([`designer-studio-v1.md`](../specs/designer-studio-v1.md) section 8).

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

Metrics include startup/first frame/TTI (time to interactive), `input_to_handler`, `handler_to_paint`,
`interaction_to_next_paint` p50/p95/p99 (50th/95th/99th percentile), frame time/jank, resource wait/cache, errors/boundary
catches, memory, idle presents. The report has raw metrics + category/overall scores.

Use raw metrics for decisions; don't chase the score.

Latency gate: `tessera headless --mode audit --profile desktop --script FILE` times each input step on a real monotonic clock, writes `out/latency-audit.json` (p50/p95/max plus violations), embeds it in `report.json`, and exits `1` on a threshold violation. Profiles are `desktop`, `mobile`, `ci`, or a custom `.json` file; see [headless-automation-v1.md, section 6.10](../specs/headless-automation-v1.md). Use `ci` on noisy shared runners.

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
- The custom-element snapshot includes element id, state, draw stats, and last event.
- Structural mode records draws without a GPU; golden mode runs compiled shaders on the software path.
- See [`custom-elements-v1.md`](../specs/custom-elements-v1.md).

### 8.2 App interaction/platform

- Text: `type`, `assertText`, `assertSelection`; IME composition scripts.
- Scroll/gestures: `scroll`/drag scripts; assert offset/visible item.
- Navigation: `navigate`, `back`, `assertRoute`; restore/deep-link scripts.
- Overlays: `openOverlay`, `closeOverlay`; assert focus/top layer/dismiss.
- Forms: assert field errors/valid/submitting; focus first invalid.
- Accessibility: semantics snapshots and platform smoke tests.

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
| Pixel diff but structure same | text/AA (anti-aliasing)/DPI/font; golden mode setting or its comparison tolerance |
| Audit noisy | profile/machine mismatch; compare against a baseline recorded with the same profile |

## 11. Status

- Specs only; the headless harness and `automation.manifest.json` generation are not implemented.
- IDs are assigned at build time; trace and audit are opt-in and their output is redacted of private data.
