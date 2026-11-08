# ADR-027: Numeric automation IDs, headless harness, and Lighthouse-style audits

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (deterministic headless golden tests), [ADR-017](ADR-017-input-and-fixed-step-runtime.md) (input and fixed-step runtime loop), [ADR-022](ADR-022-diagnostics-and-profiling.md) (diagnostics and profiling), [ADR-025](ADR-025-error-boundaries.md) (error boundaries)

## Context

Coding agents and traces need stable identity. Runtime pointers/NodeIds change every run,
coordinates break with layout changes, and string paths are fragile. Headless tests must drive
the real UI deterministically without a window/GPU/OS loop. Diagnostics need user-visible
response-time metrics, not just counters or vendor GPU utilization.

## Decision

1. **Numeric IDs:** `UiId` and `ComponentId` are `uint64_t`, assigned deterministically at
   build time; `NodeId` is the runtime instance. JSON encodes them as decimal strings.
2. **Compiler manifest:** `generated/automation.manifest.json` maps numeric IDs to debug paths,
   kinds, components, and source spans. Collisions are hard errors; explicit `automationId`
   pins a target. List items use `(UiId, ItemKey)`.
3. **Scene/snapshot API:** nodes carry `(NodeId, UiId, ComponentId)`; snapshots and queries use
   numeric IDs; actions go through the normal input path.
4. **Headless harness:** deterministic fake HAL (hardware/OS abstraction layer) + fixed clock/size/DPI/fonts/locale; structural,
   [golden](../docs/reference/glossary.md) (software/[lavapipe](../docs/reference/glossary.md)), GPU smoke, and audit modes; JSON scripts; report/snapshot/trace.
5. **Agent protocol:** line-delimited JSON over local stdin/stdout or socket; serialized
   commands; decimal-string IDs; timeouts; no network exposure.
6. **Lighthouse-style audits:** measure startup, interaction latency (`input_to_handler`,
   `handler_to_paint`, `interaction_to_next_paint` p50/p95/p99), rendering, resources,
   reliability, accessibility, memory/power; report raw metrics + category/overall scores.
7. **CI (continuous integration):** structural tests on PRs, goldens on software/lavapipe, audits with thresholds, GPU
   smoke only; no GPU baseline updates.
8. **Privacy/overhead:** automation/audit disabled by default in release; no PII (personally identifiable information); redaction;
   no pixel/output changes when disabled.

## Consequences

### Positive

- Agents can target stable IDs and reason from structured snapshots/traces.
- Headless tests are deterministic and GPU/OS-independent.
- Response-time regressions are visible with raw metrics and scores.
- One manifest correlates UI, compiler, traces, and reports.

### Negative / risks

- Hash-based IDs change on source moves/renames; explicit `automationId` is needed for stable
  long-lived targets.
- 64-bit IDs need string encoding in JSON.
- Headless/audit harness is additional infrastructure.
- Audit scores can be gamed; raw metrics must remain primary.
- Trace/screenshot data can contain PII; redaction is required.

## Alternatives considered

- **Use NodeId/pointers only.** Rejected: unstable across runs.
- **Use string paths/coordinates.** Rejected: brittle and expensive.
- **Sequential build IDs.** Rejected: adding a node shifts subsequent IDs.
- **Real app + UI automation only.** Rejected: slow, flaky, OS/GPU-dependent.
- **Web Lighthouse directly.** Rejected: native app; wrong metrics/assumptions.
- **Score-only diagnostics.** Rejected: hides raw latency/memory regressions.

## Follow-up

- Implement numeric IDs/manifest in the compiler/IR.
- Add snapshot/query/action APIs and agent protocol.
- Build the headless harness and audit runner.
- Add trace correlation and report schemas.
- Add CI thresholds/baselines and redaction tests.
