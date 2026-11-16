# Platform modules: networking, notifications, and security

Part of the [services design set](README.md). Sections 5, 6 and 8 are targets only: none of these
modules has code yet, and the security rules apply to every part of the set.

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

## 8. Security and privacy

- Least privilege: request only needed permissions.
- Validate all external input (deep links, notification payloads, network data).
- Redact tokens/paths/user data in logs/crash dumps.
- Telemetry is opt-in and aggregated ([ADR-022](../../../decisions/ADR-022-diagnostics-and-profiling.md), diagnostics and profiling); no PII (personally identifiable information) by default.
- See [`security.md`](../security.md).
