# Error boundaries v1

Status: proposed (Big Step C/F). Decision: ADR-025. Supersedes the "no error boundaries" V1
limit in ADR-007 and the component-model text that rejected them.

An error boundary isolates a component subtree. It catches synchronous exceptions from
generated component code, shows a fallback, and prevents one failing widget from crashing the
app. Async errors are values routed to the owning boundary; they are not thrown across threads.

## 1. Semantics

### Caught by the boundary

- exceptions during child **build/update**
- exceptions during child **effect** execution/cleanup
- exceptions during child **event handler** dispatch
- exceptions from generated style/layout/paint callbacks inside the boundary
- `Error` values explicitly reported via `ErrorBoundary::report(Error)`

### Not caught

- errors in the boundary's own `onError` handler or fallback subtree (propagate to parent/root)
- fatal errors: OOM/`bad_alloc`, stack overflow, abort, process-level failures
- cancellation/timeout (not errors unless the caller treats them as such)
- platform/GPU/device-loss failures (HAL/runtime handles them)
- exceptions thrown across C ABI/JNI/ObjC/Wasm (forbidden; convert to `Error` values)
- unhandled async exceptions on worker threads (never throw across threads; deliver `Error`)

Rule: **no silent catch**. Every caught error is reported once to diagnostics and/or `onError`.

## 2. Component API

```tsx
<ErrorBoundary
  fallback={<ErrorView />}
  onError={(error) => report(error)}
  resetKey={routeId}
  maxAutoRetries={0}
>
  <Child />
</ErrorBoundary>
```

| Prop | Default | Notes |
|---|---|---|
| `fallback` | required | Node subtree shown after a catch; accessible, no raw secrets |
| `onError` | optional | Receives `Error`; default logs via diagnostics |
| `resetKey` | optional | Change clears the error and remounts the child |
| `maxAutoRetries` | `0` | Auto-retry is off by default; manual retry is preferred |
| `catchEvents` | `true` | Catch event-handler exceptions; `false` for strict debugging |

V1 supports error boundaries; they are no longer in the rejected-pattern list. Fallback UI is a
normal component subtree; it may itself use components but not the same boundary.

## 3. IR shape

`ErrorBoundary` is a collection/wrapper node, not a visual scene node:

```json
{
  "id": "node::err",
  "kind": "ErrorBoundary",
  "props": {
    "resetKey": { "kind": "ref", "id": "state::route" },
    "maxAutoRetries": 0,
    "catchEvents": true
  },
  "events": { "onError": "bind::on_error" },
  "fallback": { "kind": "View", "id": "node::fallback", "children": [] },
  "children": [
    { "kind": "View", "id": "node::child", "children": [] }
  ]
}
```

Rules:

- `fallback` is required and must be a node subtree.
- `children` is exactly one subtree in V1 (wrap siblings in a `View`).
- `onError` is optional; if present it must resolve to a binding.
- `resetKey` is optional; any value change resets the boundary.
- Nested boundaries are allowed; the nearest boundary catches first.
- The boundary itself is not painted; it does not affect layout.

## 4. State machine

```text
Normal --catch--> Fallback --reset/resetKey--> Normal
   ^                  |
   |                  +-- fallback throws --> Parent boundary/Root
   +-- retry (manual) --+
```

State:

```cpp
struct ErrorBoundaryState {
    bool has_error = false;
    Error error;
    std::uint32_t retry_count = 0;
    std::uint64_t reset_generation = 0;
};
```

Rules:

- On catch: detach/freeze the child subtree, cancel child-owned tasks/animations, clear refs,
  show fallback, record diagnostics.
- Reset: clear the error, dispose the failed subtree, rebuild the child, increment generation.
- `resetKey` change resets automatically.
- Retry is manual by default; `maxAutoRetries` > 0 must be bounded and logged.
- Loop protection: repeated catches in a short window disable auto-retry and report
  `TESSERA_BOUNDARY_LOOP`; do not spin.
- If fallback/onError throws, propagate to the nearest parent boundary/root handler.
- Unmount cancels pending errors; no callback after teardown.

## 5. Exception policy and codegen

- Exceptions are allowed at **API/component boundaries**, not in frame-critical loops.
- Generated component entry points (build/update/effect/event) are guarded by the boundary;
  codegen emits a guard at the boundary scope, not a try/catch per line.
- Recoverable hot paths use `Error`/`std::expected` (or `StatusOr`); exceptions are a safety net.
- No throw across C ABI/JNI/ObjC/Wasm; convert to `Error` and report.
- Do not mark boundary guard functions `noexcept`; a throw inside `noexcept` terminates.
- If a build disables exceptions, error boundaries use explicit `Error` results; arbitrary
  throws cannot be caught. V1 generated code assumes exceptions are enabled.
- Boundary guards must preserve deterministic behavior and not swallow cancellation.

## 6. Async errors

- Async tasks return `Result<T, Error>`; they never throw across threads.
- A task's error handler reports to the owning boundary:
  `boundary.report(std::move(error))`.
- The report is delivered on the UI thread at a frame boundary.
- If the boundary/child is gone, drop the report with a diagnostic (no stale callback).
- Cancellation is not an error; do not show the fallback for cancellation.
- Timeout is an error only if the caller chooses to surface it.

## 7. Diagnostics and privacy

Codes:

```text
TESSERA_BOUNDARY_CAUGHT
TESSERA_BOUNDARY_FALLBACK_FAILED
TESSERA_BOUNDARY_LOOP
TESSERA_BOUNDARY_RESET
TESSERA_BOUNDARY_ASYNC
```

- Record boundary/component id, error code/message, source span, retry count, generation.
- Stack traces are debug/diagnostic only, never user-facing by default.
- Redact secrets/PII from messages/logs/crash dumps.
- `onError` runs on the UI thread; it must not block or throw.
- Telemetry is opt-in; no raw user content.

## 8. Resource and lifetime rules

- On catch, cancel child-owned tasks/animations, release refs, detach the subtree.
- Do not destroy the boundary/parent; only the failed child subtree is isolated.
- Fallback owns its nodes; dispose on reset/unmount.
- GPU resources follow the normal retirement rules (after frames in flight).
- Async completions for the failed child are discarded or routed to the boundary.
- No use-after-free from a stale task/event.

## 9. Testing

- Throw in child build/update/effect/event handler -> fallback shown, error logged.
- Nested boundaries: nearest catches; parent unaffected.
- Fallback throws -> parent/root catches; no infinite loop.
- Reset/retry/resetKey -> child rebuilds; state/resource cleanup correct.
- Async error report -> fallback; cancellation does not.
- Fatal/OOM/device-loss -> not caught by component boundary; root handler.
- No throw across FFI; error values only.
- Loop protection/rate limiting.
- Accessibility of fallback; no focus trap/regression.
- Redaction/telemetry tests.
- Golden fallback rendering at fixed theme/DPI.

## 10. Open questions

- Whether event-handler exceptions should be caught by default or opt-in.
- Auto-retry policy and backoff.
- Root boundary default UI and crash-report integration.
- No-exceptions build support and codegen strategy.
- Which errors are fatal vs recoverable per subsystem.
- Boundary-aware async ownership API.
- Error boundary interaction with suspense/loading (future).
