# Error model v1

Status: proposed. Applies to configuration, compiler, runtime, HAL, GPU, async, and diagnostics.

## 1. Principles

- Errors are explicit values at recoverable boundaries; exceptions only at API/component
  boundaries, where an error boundary catches them (see §11).
- Every error has a stable code, severity, message, and source location where applicable.
- User-facing messages are actionable; internal details are attached for diagnostics.
- No silent failure: fallbacks are documented and recorded.
- Errors never block the UI thread; async failures are delivered as results/events.
- Sensitive data is redacted from messages/logs.

## 2. Error shape

```cpp
enum class Severity { Trace, Debug, Info, Warn, Error, Fatal };

struct Error {
    std::string code;        // TESSERA_*
    Severity severity;
    std::string message;     // user-facing
    std::string detail;      // internal/debug (redacted)
    SourceSpan source;       // optional
    std::string remediation; // optional suggested fix
};
```

APIs return `std::expected<T, Error>` (or a project `StatusOr<T>` on toolchains without it)
for recoverable failures. Exceptions are allowed for construction/programming errors at
boundaries (invalid arguments, out-of-memory) and are translated to `Error` where a result is
expected.

## 3. Code families

| Prefix | Area | Examples |
|---|---|---|
| `TESSERA_CFG_` | project/runtime config | unknown field, invalid value, path escape |
| `TESSERA_JSX_` | JSX/TSX syntax | invalid token, unsupported construct |
| `TESSERA_REACT_` | hooks/rules | hook order, missing deps, custom hook |
| `TESSERA_IR_` | IR validation | duplicate id, unresolved ref, version |
| `TESSERA_CG_` | codegen | unsupported node, identifier collision |
| `TESSERA_MERGE_` | protected regions | duplicate/missing marker, mismatch |
| `TESSERA_CSS_` | stylesheet | parse, unknown property, var cycle |
| `TESSERA_LAYOUT_` | layout | invalid constraint, measure failure |
| `TESSERA_TEXT_` | text | missing font/glyph, shaping failure |
| `TESSERA_RHI_` | RHI/backend | device/feature unavailable, validation |
| `TESSERA_HAL_` | platform | window/display/input/power failure |
| `TESSERA_ASYNC_` | async | cancelled, timeout, queue overflow |
| `TESSERA_NET_` | networking | TLS, timeout, HTTP status |
| `TESSERA_IO_` | storage/filesystem | open/read/write/atomic rename |
| `TESSERA_DIAG_` | diagnostics | sink failure, redaction |
| `TESSERA_SEC_` | security | path traversal, invalid payload |

Codes are stable strings; do not renumber. Add new codes in the owning module and document
them.

## 4. Severity

- `Trace/Debug`: development detail; not user-facing.
- `Info`: normal lifecycle/fallback.
- `Warn`: degraded behavior or recoverable problem.
- `Error`: operation failed; app continues.
- `Fatal`: cannot continue safely; controlled shutdown/crash path.

Recoverable fallbacks (60 Hz/1.0 scale/no VRR) are `Info`/`Warn` and recorded in metrics.

## 5. Propagation

```text
platform/GPU -> translated Error -> completion queue/event
  -> app/state -> UI message/log/metric
```

- No throwing across C ABI/JNI/Wasm boundaries.
- Async errors are values delivered to the owner; cancellation is not an error unless the
  caller treats it as one.
- Fatal errors trigger a bounded shutdown/crash handler; no infinite waits.
- Errors never unwind through user callbacks.

## 6. User messages

- State what failed, why (briefly), and what to do next.
- Do not include raw OS/Vulkan codes in the primary message; attach them as detail.
- Include file/line/column for config/compiler errors.
- Use stable message keys for localization later; V1 English.

## 7. Redaction

- Never log tokens, passwords, secrets, full URLs with credentials, or user content by default.
- Paths may be redacted to project-relative form.
- Crash dumps/telemetry follow the diagnostics privacy rules (ADR-022/security.md).
- Redaction is tested.

## 8. Compiler errors

- Every diagnostic has code/severity/message/source span/fix.
- Warnings can be errors in CI (`-Werror` equivalent).
- A component with errors is not partially generated.
- Build output groups by file and includes a summary.

## 9. Runtime/GPU errors

- Device loss/out-of-memory are recoverable: recreate resources or fall back.
- Validation-layer messages are diagnostics in debug; not user-facing.
- Missing features -> documented fallback, not a crash.
- Fatal GPU failure -> controlled shutdown; report code/detail.

## 10. Testing

- Unit tests for error codes/messages/redaction.
- Compiler fixtures assert exact codes/spans.
- Async cancellation/timeout/overflow tests.
- HAL failure injection (window/display/power) tests.
- No test asserts on raw OS/Vulkan text.

## 11. Error boundaries

- Error boundaries catch synchronous exceptions from generated component code (build/update/
  effect/event/generated callbacks). See [`error-boundaries-v1.md`](error-boundaries-v1.md).
- Async errors are values (`Result<T, Error>`) reported to the owning boundary; never throw
  across threads.
- Do not catch fatal/OOM/stack overflow/cancellation/platform/GPU errors in a component
  boundary; route them to the root/app handler.
- No silent catch: report once to diagnostics/onError; redact secrets.
- Fallback/onError failures propagate to the nearest parent boundary/root.
- Exceptions remain forbidden across C ABI/JNI/ObjC/Wasm boundaries.

## 12. Related

- [`diagnostics.md`](../design/diagnostics.md)
- [`diagnostics-config-v1.md`](diagnostics-config-v1.md)
- [`security.md`](../design/security.md)
- [`compiler.md`](../design/compiler.md)
- [`error-boundaries-v1.md`](error-boundaries-v1.md)
