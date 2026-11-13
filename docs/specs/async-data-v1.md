# Async data, cache, and offline v1

Status: proposed (Big Step F, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step)). "V1" means the first-release scope ([AGENTS.md](../../AGENTS.md), Non-negotiables rule 8). Decision: [ADR-030](../../decisions/ADR-030-app-platform-completeness.md) (navigation, restore, shell, i18n, accessibility, async data). Companion to
[`async.md`](../design/async.md), [`services.md`](../design/services.md),
[`startup-and-loading.md`](../design/startup-and-loading.md),
[`forms-and-validation-v1.md`](forms-and-validation-v1.md), and
[`resource-lifecycle-v1.md`](resource-lifecycle-v1.md).

The async runtime ([ADR-021](../../decisions/ADR-021-async-runtime.md), [`async.md`](../design/async.md)) owns tasks; this spec owns **data**: keys, cache, fetch state, invalidation,
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

- A resource is keyed by a stable typed key (route, id, query, locale). Here a resource is a typed data record, not an asset (see section 8.1).
- `Loading` has no data; `Refreshing` keeps previous data; `Stale` may show old data with a hint.
- Epochs (a counter incremented on every key/param/cancel change, section 8.1) discard stale responses after key/params/cancellation changes.
- Data is immutable once published; UI reads snapshots, not mutable caches.
- Errors are values (`Error`, see [`error-model-v1.md`](error-model-v1.md)), localized at the UI edge; no thrown exceptions.

## 2. Cache and dedupe

- In-memory cache keyed by resource key; optional disk cache for offline/startup.
- Concurrent requests for the same key dedupe to one task.
- Cache entries have TTL (time to live)/stale policy and size budgets; eviction is deterministic.
- Cache is scoped (app/user/route); logout clears user-scoped data.
- Disk cache is versioned/encrypted for sensitive data (no classification rule is defined in this file); corruption falls back to network.
- No unbounded cache growth; metrics report hit/miss/eviction/bytes.

## 3. Fetch policy

```text
cache-first | network-first | stale-while-revalidate | network-only | cache-only
```

- Policy is explicit per resource; no hidden global behavior.
- Cancellation on route change/unmount; no callback after teardown.
- Timeouts/backoff/retry for idempotent reads; no retry storms.
- Prefetch is scheduled with priority/deadline and cancellable (see [`resource-lifecycle-v1.md`](resource-lifecycle-v1.md)).
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

- Offline detection comes from services ([`services.md`](../design/services.md)) or the HAL ([`hal.md`](../design/hal.md)); not polling the network in the loop.
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
- Accessibility announces loading/error at a bounded rate (limits in [`accessibility-v1.md`](accessibility-v1.md) section 10.1: identical messages within 250 ms, progress within 1 s).

## 7. Security and privacy

- Validate/schema-check external data; bound sizes/depth.
- TLS (transport layer security) verification; no insecure fallback; redact headers/tokens.
- Do not cache secrets in plaintext; use secure storage/expiry.
- User/tenant scoping on every cache key; logout clears.
- No PII (personally identifiable information) in logs/traces; diagnostics redact.
- Untrusted data never becomes code/paths/HTML.

## 8. Testing

- Cache hit/miss/eviction/TTL; dedupe; stale epochs.
- Fetch policies, timeout/retry/backoff, cancellation.
- Mutations/invalidation/optimistic/rollback/conflict.
- Offline/reconnect/queued flush; cache-only.
- Pagination/prefetch/dedupe/scroll preservation.
- UI states loading/refreshing/error/empty; accessibility announcements.
- Headless fake clock/network; deterministic async scheduler.
- Push entry: `set`/`update` notify observers once per key at the tick boundary (never inline, never
  on the app/worker thread); coalesced pushes deliver the latest snapshot only.
- Push ordering: last write wins per key with a monotonic version; a push supersedes an in-flight
  fetch (epoch bump plus cancellation), and the fetch's late success/error is discarded.
- Pushed entries share the bounded cache and its deterministic LRU eviction.

## 8.1 Detail design

Ownership and boundaries:

| Component | Owns | Does not own |
|---|---|---|
| `DataStore` | typed query/resource cache, invalidation, mutation state | Asset bytes/GPU resources |
| `Query<T>` | key, status, data/error, epoch, subscribers | Transport |
| `Mutation<T>` | mutation state, optimistic patch/rollback | UI focus/navigation |
| `OfflineQueue` | durable queued mutations/idempotency | Network policy |
| `async` runtime | tasks/executors/cancellation | Domain cache semantics |
| [`resource-lifecycle-v1.md`](resource-lifecycle-v1.md) | asset bytes/fonts/images/shaders/media | Typed records/queries |
| `services/networking` | HTTP/TLS/streaming | Cache keys/UI state |

Canonical rule: this spec (`async-data-v1.md`) caches **typed domain records**;
[`resource-lifecycle-v1.md`](resource-lifecycle-v1.md) caches **byte/asset resources**. Do not store the same payload in both. A query may reference an asset
id, but the asset is loaded by the resource manager.

Cache key:

```cpp
struct DataKey {
    std::string type;          // query/mutation kind
    std::string scope;         // app/user/tenant
    std::string route;         // optional
    std::string locale;        // only when formatting/query depends on it
    std::string params_hash;   // canonical typed params
};
```

- Serialize params canonically (sorted keys, stable number/string encoding) before hashing.
- Scope is mandatory; logout/tenant switch invalidates all user/tenant keys.
- Never include secrets in keys/logs; hash sensitive params.

Query state machine:

```text
Idle -> Loading -> Success
             \-> Error
Success -> Refreshing -> Success/Error (keep previous data)
Success/Error -> Stale (TTL/invalidation)
Any -> Cancelled (key change/unmount; internal, not a `DataStatus` value)
```

- Cancelled is an internal transition, not an observable `DataStatus`: after a key change the
  resource reports the new key's status, and on unmount there is no subscriber to observe it.
- `loading` has no data; `refreshing` keeps previous data; `error` may keep previous data.
- Every key/param/cancel change increments `epoch`; stale completions are discarded.
- Concurrent requests for one key dedupe; subscribers share the in-flight result.
- Cache policy per key: `cache-first`, `network-first`, `stale-while-revalidate`,
  `network-only`, `cache-only`; TTL and size are explicit.

Mutations:

```text
Idle -> OptimisticApplied -> Submitting -> Success
                         \-> Rollback (error/cancel)
```

- Optimistic patches are reversible; store the previous value/patch id.
- On success: invalidate/update declared keys; on failure: rollback then surface error.
- Mutation epoch prevents an older success from overwriting a newer mutation.
- Offline queue entries are bounded, idempotent, and scoped; replay preserves order per key.
- Conflict policy is explicit: server-wins, client-merge, or prompt; no silent overwrite.

Failure/edge cases:

- Corrupt disk cache -> discard, refetch, diagnostic.
- Offline + no cache -> error/empty state, not fake success.
- Logout during request -> cancel/ignore, clear scoped data.
- Duplicate pagination pages -> dedupe by stable item key, not index alone.
- Refresh failure -> keep old data and show retry/stale indicator.
- Streaming and subscription transports are not V1 (define ordering and backpressure separately);
  the V1 push entry `DataStore::set/update` lets an app feed a query from its own source (issue #233).

App-fed push entry (issue #233):

- `DataStore::set(key, value)` and `DataStore::update(key, fn)` are the only V1 push entry; they add
  no transport (WebSocket/SSE/GraphQL subscriptions stay after V1).
- A push writes the cache entry, marks it `Success` (fresh, `updated_at` from the store clock) and
  bumps the key's monotonic `version` (0 until the first write); last write wins per key,
  independent of the calling thread.
- A push also bumps the key's `epoch` and clears/cancels the in-flight fetch and its
  `CancellationToken` (section 1); the fetch's late `resolve`/`fail` returns false and never
  overwrites the pushed value. A fetch started after the push is a new epoch and follows normal
  fetch ordering.
- Observers are notified once per key per tick, coalesced across pushes in that tick, when the
  frame tick drains the completion queue on the UI thread. A push never calls an observer inline
  and never blocks the caller beyond the store mutex. Delivery uses the bounded completion queue:
  if it is full the notification stays pending and the next write re-arms it (data is not lost, no
  unbounded retry).
- `update` reads the current typed value outside the lock and retries on a version conflict, so a
  concurrent push is not lost; it refuses a key that holds another type.
- Pushed entries share the entry bound and deterministic LRU eviction. Eviction does not notify:
  an observer of an evicted key reads `Idle` until the key is pushed again.

Gap-closure decisions (ids G-n are gaps and Q-n are open questions, listed in [`app-completeness-cross-check-v1.md`](../architecture/app-completeness-cross-check-v1.md); resolutions in [`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md)):

- **G-20/Q-19/Q-20**: keys are canonical JSON + hash with mandatory scope; disk cache is
  optional and encrypted for sensitive data; cross-window invalidation uses store events.
  Offline queue is capped at 100 mutations/1 MiB. Default conflict policy is server-wins with a
  surfaced conflict error; merge is opt-in per mutation.
- **G-21**: V1 supports cursor/page pagination; streaming/subscriptions/GraphQL transports are deferred (no release assigned). The app-fed push entry `DataStore::set/update` is in V1 (#233): last write wins per key with a monotonic version, a push supersedes and cancels an in-flight fetch (its late result is discarded), observers are notified once per key per tick on the UI thread, and pushed entries obey the cache bound.
  Pagination dedupes by stable item key and preserves anchor/scroll state.

## 9. Deferred/open questions

- Query language/schema generation vs hand-written resources.
- Disk cache migration/compaction and key rotation.
- Prefetch budgets vs power policy.
- Streaming/subscriptions (after V1; no release assigned).
- Cross-window cache sharing beyond invalidation events.
