# Services: i18n, navigation, storage, networking, notifications

Status: design for Big Step F. `src/services/*` are placeholders.

Services are app-facing capabilities that are not part of the renderer. They follow the same
runtime rules: async, cancellable, bounded, no UI-thread blocking, and no hidden global state.

## 1. Common contract

```cpp
template <typename T>
using Result = std::expected<T, ServiceError>;  // or StatusOr<T> on C++20 toolchains

class Service {
public:
    virtual ~Service() = default;
    virtual void shutdown() = 0;
};
```

- Every async operation returns a cancellable task/result.
- Errors are values (`std::expected`/`StatusOr`), not exceptions in hot paths.
- Results are delivered on the UI thread through the completion queue.
- Services never touch scene nodes directly; the app maps results to state.
- Configuration is explicit (no environment-variable magic in production).
- Secrets are never logged; diagnostics redact.

## 2. Internationalization (i18n)

- ICU for locale data, plural rules, number/date formatting, segmentation.
- Message catalogs keyed by stable ids; no concatenated sentences.
- Support RTL/layout direction, font fallback, and text expansion.
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
- Encryption/secrets use the platform keystore where available; never plaintext logs.
- Tests: atomicity, migration, corruption recovery, concurrent access, quota limits.

## 5. Networking

- Async HTTP client with cancellation, timeouts, and bounded concurrency.
- TLS verification on by default; no insecure fallback.
- Retries with backoff for idempotent requests; no retry storms.
- Streaming/chunked responses for large data; backpressure.
- Offline detection and cache policy are explicit.
- Proxy/platform network stack integration where required.
- Secrets/tokens are never logged; redact headers/query params.
- Tests: timeout, cancel, retry, TLS failure, offline, large body, redirect policy.

## 6. Notifications

- OS notifications with permission flow; no notification without user consent.
- Actions/categories map to app callbacks; payloads are validated.
- In-app toasts/badges are UI components, not OS notifications.
- Rate-limit/coalesce; respect quiet hours/do-not-disturb where exposed.
- Clicking a notification restores the relevant route/state.
- Tests: permission denied, action payloads, deduplication, restore.

## 7. Background and lifecycle

- Services react to suspend/resume/background/foreground via normalized events.
- Long-running work is cancelled or checkpointed on suspend.
- Network/file handles are released in background where required.
- No background work that violates platform policy; document per-platform limits.
- Tests: suspend during load, resume/restore, cancellation, duplicate callbacks.

## 8. Security and privacy

- Least privilege: request only needed permissions.
- Validate all external input (deep links, notification payloads, network data).
- Redact tokens/paths/user data in logs/crash dumps.
- Telemetry is opt-in and aggregated (ADR-022); no PII by default.
- See [`security.md`](security.md).

## 9. Status

Services are post-V1 (Big Step F). The interfaces above are targets; implementation order:
storage/settings, i18n, navigation, networking, notifications.

## 10. Open questions

- Which HTTP/TLS stack (platform vs vendored) and static-link implications.
- Localization catalog format and build pipeline.
- Navigation integration with the compiler/IR.
- Background execution policy on mobile.
- Secret storage abstraction across platforms.
