# Async data, cache, and offline v1

Status: proposed (Big Step F). Decision: ADR-030. Companion to
[`async.md`](../design/async.md), [`services.md`](../design/services.md),
[`startup-and-loading.md`](../design/startup-and-loading.md), and
[`forms-and-validation-v1.md`](forms-and-validation-v1.md).

The async runtime owns tasks; this spec owns **data**: keys, cache, fetch state, invalidation,
retry, offline, and UI integration.

## 1. Resource model

```cpp
enum class DataStatus { Idle, Loading, Refreshing, Success, Error, Stale };

struct Resource<T> {
    DataStatus status;
    std::optional<T> data;
    std::optional<Error> error;
    std::uint64_t epoch;
    std::optional<TimePoint> updated_at;
};
```

- A resource is keyed by a stable typed key (route, id, query, locale).
- `Loading` has no data; `Refreshing` keeps previous data; `Stale` may show old data with a hint.
- Epochs discard stale responses after key/params/cancellation changes.
- Data is immutable once published; UI reads snapshots, not mutable caches.
- Errors are values, localized at the UI edge; no thrown exceptions.

## 2. Cache and dedupe

- In-memory cache keyed by resource key; optional disk cache for offline/startup.
- Concurrent requests for the same key dedupe to one task.
- Cache entries have TTL/stale policy and size budgets; eviction is deterministic.
- Cache is scoped (app/user/route); logout clears user-scoped data.
- Disk cache is versioned/encrypted where sensitive; corruption falls back to network.
- No unbounded cache growth; metrics report hit/miss/eviction/bytes.

## 3. Fetch policy

```text
cache-first | network-first | stale-while-revalidate | network-only | cache-only
```

- Policy is explicit per resource; no hidden global behavior.
- Cancellation on route change/unmount; no callback after teardown.
- Timeouts/backoff/retry for idempotent reads; no retry storms.
- Prefetch is scheduled with priority/deadline and cancellable (see resource lifecycle).
- Pagination uses cursor/page keys; prefetch next page near the end.
- Dependencies are explicit; dependent resources revalidate on key changes.

## 4. Mutations and invalidation

- Mutations are separate async commands with their own status/error.
- On success, update/invalidate related resource keys; exact list is declared.
- Optimistic updates are opt-in with rollback on failure.
- Double-submit is deduped; mutation epoch prevents stale success overwriting newer state.
- Conflict policy (server wins/client merge) is explicit.
- Cache invalidation is scoped; logout/tenant switch clears all user data.
- No mutation callback after unmount/window close.

## 5. Offline

- Offline detection comes from services/HAL; not polling the network in the loop.
- Read-only cached data may be shown with an offline/stale indicator.
- Queueable mutations are explicit, bounded, and idempotent; conflict/error surfaced.
- Reconnect triggers revalidation/queued flush with backoff and cancellation.
- Offline state is part of the UI state, not a special app mode.
- Cache-only mode is explicit; never silently show empty as success.

## 6. UI integration

- Loading: skeleton/progress; keep layout stable; no fake progress.
- Refreshing: keep old data; show subtle indicator; do not clear the screen.
- Error: actionable retry/offline; preserve previous data when possible.
- Empty: distinct from loading/error; no false “no data” while loading.
- Pagination: append with stable keys; dedupe items; preserve scroll/selection.
- Detail screens use the list item as placeholder while fetching.
- Forms/validation use async data only through explicit resources; no hidden fetch in render.
- Accessibility announces loading/error at a bounded rate.

## 7. Security and privacy

- Validate/schema-check external data; bound sizes/depth.
- TLS verification; no insecure fallback; redact headers/tokens.
- Do not cache secrets in plaintext; use secure storage/expiry.
- User/tenant scoping on every cache key; logout clears.
- No PII in logs/traces; diagnostics redact.
- Untrusted data never becomes code/paths/HTML.

## 8. Testing

- Cache hit/miss/eviction/TTL; dedupe; stale epochs.
- Fetch policies, timeout/retry/backoff, cancellation.
- Mutations/invalidation/optimistic/rollback/conflict.
- Offline/reconnect/queued flush; cache-only.
- Pagination/prefetch/dedupe/scroll preservation.
- UI states loading/refreshing/error/empty; accessibility announcements.
- Headless fake clock/network; deterministic async scheduler.

## 9. Open questions

- Query language/schema generation vs hand-written resources.
- Disk cache format/encryption and migration.
- Offline mutation conflict resolution.
- Cross-window cache sharing/invalidation.
- Prefetch budgets vs power policy.
- GraphQL/streaming/subscription support (out of scope for v1).
