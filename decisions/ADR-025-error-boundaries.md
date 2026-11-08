# ADR-025: Error boundaries and exception isolation

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-007](ADR-007-compiler-ir.md) (compiler intermediate representation), [ADR-021](ADR-021-async-runtime.md) (async runtime), [ADR-022](ADR-022-diagnostics-and-profiling.md) (diagnostics and profiling), [ADR-024](ADR-024-native-first-boundaries.md) (native-first boundaries)
- Supersedes: the "no error boundaries" [V1](../docs/reference/glossary.md) limit in [ADR-007](ADR-007-compiler-ir.md) and the text in
  [`docs/specs/component-model-v1.md`](../docs/specs/component-model-v1.md) that rejected error boundaries.

## Context

Tessera generates C++ component code with user/developer logic. A thrown exception in a
component build/update/effect/event handler must not crash the whole app or leave a broken
scene. Earlier V1 scope rejected error boundaries to keep the surface small; that is no longer
acceptable for production apps.

C++ exceptions also interact with async, bridges, and frame-critical code: throwing across
JNI (Java Native Interface)/ObjC/Wasm/thread boundaries is unsafe, and try/catch per line would hurt codegen and
determinism.

## Decision

1. **Error boundaries are a V1 component/IR feature.** A boundary wraps one subtree and owns a
   fallback UI.
2. **Catch synchronous exceptions** from child build/update/effect/event-handler/generated
   style-layout-paint callbacks.
3. **Async errors are values**, not exceptions: tasks return `Result<T, Error>` and report to
   the owning boundary on the UI thread.
4. **Do not catch** fatal/OOM/stack overflow/abort, cancellation/timeout, platform/GPU/device
   loss, errors in the boundary's own fallback/onError, or exceptions crossing FFI (foreign function interface)/thread
   boundaries.
5. **No silent catch.** Report once to diagnostics/onError; redact secrets; no raw stack to
   users.
6. **Nested boundaries**: nearest catches; fallback failure propagates to parent/root. A root
   boundary/crash handler catches leftovers.
7. **Reset/retry** is explicit (`resetKey`, manual retry); auto-retry is off by default and
   bounded with loop protection.
8. **Codegen guards boundary entry points**, not every line. Hot paths still avoid exceptions;
   recoverable paths use `Error`/`std::expected`.
9. **No throw across C ABI (application binary interface)/JNI/ObjC/Wasm**; convert to `Error` values.
10. **Resource cleanup**: cancel child tasks/animations, release refs, detach child, retire GPU
    resources after frames in flight.
11. **Tests**: fallback/retry/nested/async/event/fallback-failure/loop/redaction/cleanup.

## Consequences

### Positive

- One failing widget cannot crash the app.
- Clear fallback/retry UX and diagnostics.
- Async/bridge boundaries stay exception-safe.
- Error isolation is testable and scoped.

### Negative / risks

- Exceptions can hide bugs if overused; logging/escalation is required.
- Codegen guards add complexity; per-line try/catch is forbidden.
- C++ exception cost/ABI and no-exceptions builds need a policy.
- Boundary/fallback failures need a parent/root handler.
- Async ownership must be boundary-aware.

## Alternatives considered

- **No boundaries; crash on any exception.** Rejected: unacceptable production UX.
- **Global try/catch only.** Rejected: loses subtree isolation/fallback.
- **`std::expected` only, no exceptions.** Good for recoverable paths, but cannot catch
  arbitrary developer throws; keep as the primary model and add boundaries as a safety net.
- **Platform crash handler only.** Rejected: process-level, no UI recovery.
- **React-style render-only boundaries.** Too narrow for C++; we also isolate effects/events,
  while async remains value-based.

## Follow-up

- Implement `ErrorBoundary` in IR (intermediate representation)/codegen and the runtime.
- Add diagnostics codes and redaction tests.
- Add root boundary/crash-report integration.
- Document no-exceptions build behavior.
- Add fallback/retry UX guidance.
