# Services: i18n, navigation, storage, networking, notifications

Status: design for [Big Step F](../../AGENTS.md#big-steps-group-work-dont-micro-step) (production breadth). `src/services/*` are placeholders.

Services are app-facing capabilities that are not part of the renderer. They follow the same
runtime rules: async, cancellable, bounded, no UI-thread blocking, and no hidden global state.

Normative companions: [`navigation-v1.md`](../specs/navigation-v1.md),
[`i18n-v1.md`](../specs/i18n-v1.md), [`platform-shell-v1.md`](../specs/platform-shell-v1.md),
[`async-data-v1.md`](../specs/async-data-v1.md), and
[`state-restoration-v1.md`](../specs/state-restoration-v1.md).

## 1. Common contract

```cpp
template <typename T>
using Result = std::expected<T, ServiceError>;

class Service {
public:
    virtual ~Service() = default;
    virtual void shutdown() = 0;
};
```

- Every async operation returns a cancellable task/result.
- Errors are values (`std::expected`), not exceptions in hot paths.
- Results are delivered on the UI thread through the completion queue ([`async.md`](async.md)).
- Services never touch scene nodes directly; the app maps results to state.
- Configuration is explicit (no environment-variable magic in production).
- Secrets are never logged; diagnostics redact.

### 1.1 Implemented shape (AF1, issue #173)

ADR-050 section 4 fixes the contract: `virtual runtime::Task<Status> start(StartContext&)` and
`virtual runtime::Task<void> stop(StopContext&)`. That signature is what landed, but **the body
cannot literally be a coroutine** with the in-tree `runtime::Task`. `Task::promise_type`'s
constructor takes the scheduling context (`runtime::TaskContext`) as its *first* parameter, and a
member coroutine's implicit object parameter already occupies that position, so the promise
constructor is never matched and the member body fails to compile as a coroutine. Verified with MSVC
14.44; the same holds for any conforming compiler.

The working shape keeps the virtual signature and moves the coroutine to a context-first free or
static function:

```cpp
runtime::Task<ServiceStatus> start(StartContext& ctx) override { return run_start(ctx, *this); }

static runtime::Task<ServiceStatus> run_start(StartContext ctx, MailService& self) {
    MailStore& store = *ctx.services->find<MailStore>();
    co_await store.load();
    co_return ServiceStatus::Ok;
}
```

`StartContext` and `StopContext` convert to `runtime::TaskContext`, so the free body's promise takes
its queue, priority and cancellation token straight from the context. This is a mechanical
consequence of the existing coroutine runtime (see [`async.md`](async.md)), not a change to the
service contract.

What AF1 implements, and what it deliberately leaves:

| ADR-050 section 4 item | AF1 |
|---|---|
| 1 — topological start, reverse stop | yes; a cycle or a missing id fails `prepare()` |
| 2 — start phases (`Runtime`/`Background`/`Idle`), `required` gating | no: `required` is recorded and its failure reported through `required_failure()`, which AF3 polls; phase ordering is AF3 |
| 3 — one `TaskScope` per service | no: AF3 |
| 4 — bounded non-blocking shutdown | yes for stop (500 ms per service inside a 2 s total). Start ships **without** a deadline: the ADR specifies no start budget, and the mechanism is implemented and tested so a future ADR line can enable it |
| 5 — errors are values, exceptions caught at the boundary | yes: an escaped exception becomes `Failed`, never reaches the frame loop |
| 6 — virtual dispatch at the service boundary | yes |

Two readings of underspecified transitions, both conservative and both stated in the header:

- **Only a service that reached `Started` is stopped.** A service whose start failed cleans up after
  itself, because ADR-050's diagram defines no `stop()` for a service that never ran.
- **A stop that throws is `Abandoned`, not `Stopped`.** The diagram gives `Stopping` only those two
  outcomes, and an unconfirmed stop is not a confirmed one.

## 2. Internationalization (i18n)

- ICU (International Components for Unicode) for locale data, plural rules, number/date formatting, segmentation.
- Message catalogs keyed by stable ids; no concatenated sentences.
- Support RTL (right-to-left)/layout direction, font fallback, and text expansion.
- Locale is a runtime input (Configuration/NSLocale/etc.) and a style/query input.
- Missing keys fall back to a default locale and emit a diagnostic.
- Deterministic mode pins locale/ICU data; no system locale leakage in goldens.
- Tests: plural categories, RTL, long strings, date/number formats, missing keys.

## 3. Navigation

- Routes are declarative; a stack model with push/pop/replace/reset.
- Deep links map to routes with validated parameters; see
  [`deep-linking-v1.md`](../specs/deep-linking-v1.md).
- Back handling is platform-aware (Android back, browser history, keyboard shortcuts).
- State restoration: save/restore route stack and per-route state.
- Transitions are animations, not navigation semantics; reduced motion skips them.
- Modal/dialog navigation has an explicit dismiss policy.
- Tests: route matching, params, back stack, deep links, restore, unknown routes.

## 4. Storage

- Three classes: settings (small key/value), documents (app data), cache (evictable).
- Atomic writes (temp + rename); fsync policy documented per platform.
- Schema migrations are versioned and run once; failures do not corrupt data.
- Paths are sandboxed per app; no absolute/traversal paths from untrusted input.
- Large data is streamed; reads/writes happen off the UI thread.
- Encryption/secrets use the platform keystore; never plaintext logs. Keystore availability per platform is an open question (section 10).
- Tests: atomicity, migration, corruption recovery, concurrent access, quota limits.

## 5. Networking

- Async HTTP client with cancellation, timeouts, and bounded concurrency.
- TLS (transport layer security) verification on by default; no insecure fallback.
- Retries with backoff for idempotent requests; no retry storms.
- Streaming/chunked responses for large data; backpressure.
- Offline detection and cache policy are explicit.
- Proxy/platform network stack integration when the platform requires it (the platforms and conditions are not specified).
- Secrets/tokens are never logged; redact headers/query params.
- Tests: timeout, cancel, retry, TLS failure, offline, large body, redirect policy.

## 6. Notifications

- OS notifications with permission flow; no notification without user consent.
- Actions/categories map to app callbacks; payloads are validated.
- In-app toasts/badges are UI components, not OS notifications.
- Rate-limit/coalesce; respect quiet hours/do-not-disturb when the OS exposes the setting.
- Clicking a notification restores the relevant route/state.
- Tests: permission denied, action payloads, deduplication, restore.

## 7. Background and lifecycle

- Services react to suspend/resume/background/foreground via normalized events.
- Long-running work is cancelled or checkpointed on suspend.
- Network/file handles are released in background when the platform requires it (conditions not specified; see section 10).
- No background work that violates platform policy; document per-platform limits.
- Tests: suspend during load, resume/restore, cancellation, duplicate callbacks.

## 8. Security and privacy

- Least privilege: request only needed permissions.
- Validate all external input (deep links, notification payloads, network data).
- Redact tokens/paths/user data in logs/crash dumps.
- Telemetry is opt-in and aggregated ([ADR-022](../../decisions/ADR-022-diagnostics-and-profiling.md), diagnostics and profiling); no PII (personally identifiable information) by default.
- See [`security.md`](security.md).

## 9. Status

Services are post-V1 (V1 = first-release scope, see [AGENTS.md rule 8](../../AGENTS.md#non-negotiables); the work is in [Big Step F](../../AGENTS.md#big-steps-group-work-dont-micro-step)). The interfaces above are targets; implementation order:
storage/settings, navigation/restore, i18n, platform shell, networking, async data,
notifications.

## 10. Open questions

- Which HTTP/TLS stack (platform vs vendored) and static-link implications.
- Localization catalog format and build pipeline.
- Navigation integration with the compiler/IR.
- Background execution policy on mobile.
- Secret storage abstraction across platforms.
- Who marks the in-frame service-lookup region. ADR-050 section 3 item 5 forbids resolving a
  service while a frame is being updated and defines a counter for it, but the region has to be
  marked by somebody. AF1 (issue #173) ships the marker as `appfw::FrameScope` and the counters as
  `service_lookup_audit()`, and deliberately does **not** wire it into `runtime::RuntimeLoop`:
  layer 0 gains seams, not concepts (ADR-050 section 1 rule 1), and a whole-tick mark would count a
  service's own legal `start()` lookup, because a start resumes from the completion drain that runs
  inside the tick. The decision AF3 owes is whether `RuntimeLoop` marks the update/render stage
  itself or whether the app-framework layer's tick wrapper must. Nothing in AF1 depends on the
  answer; a lookup counter is only as honest as the caller marking the region, and until this is
  decided the zero-violation claim is scoped to the tests that mark explicitly.
