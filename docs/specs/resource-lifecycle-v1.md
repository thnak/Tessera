# Resource lifecycle v1: lazy, preload, lease, unload

Status: proposed (Big Step B2/F). Normative companion to
[`resources.md`](../design/resources.md), [`memory-model-v1.md`](memory-model-v1.md),
[`startup-and-loading.md`](../design/startup-and-loading.md), and
[`async-data-v1.md`](async-data-v1.md).

Resources are **lazy by default**. Only an explicit critical set is eager; preload is a
scheduled lazy load; every loaded resource has an owner/lease and an unload path.

## 1. Principles

1. **Declared, not loaded.** A resource reference does not load until requested/preloaded.
2. **Demand-driven.** Visible/first-screen resources first; everything else later.
3. **Preload is a hint.** Priority/deadline/cancellation, not a guarantee.
4. **Leases, not raw pointers.** Handles/refcounts decide lifetime.
5. **Unload is normal.** Route exit, background, logout, memory pressure, budget eviction.
6. **Bounded.** Per-class/total budgets; no unbounded caches/queues.
7. **No UI blocking.** Load/decode/upload off the UI thread; completion at frame boundary.
8. **Deterministic tests.** Fixed critical set and load order in golden mode.

Boundary: resource lifecycle owns **byte/asset resources** (fonts, images, shaders, media,
document blobs). Typed domain data/queries, query caches, mutations, and offline queues belong
to [`async-data-v1.md`](async-data-v1.md); do not store the same payload in both. `ResourceClass::Data`
is for opaque byte payloads, not a query cache.

## 2. States

```text
Declared -> Queued -> Loading -> Ready -> Active -> Idle -> Evictable -> Unloading -> Disposed
                         \-> Failed -> Retry/Evict
```

| State | Meaning |
|---|---|
| Declared | known, not requested |
| Queued | requested/preloaded, waiting for budget/priority |
| Loading | I/O/decode/upload in flight |
| Ready | available, no lease yet |
| Active | at least one lease |
| Idle | ready, no lease, cacheable |
| Evictable | cache budget/memory pressure candidate |
| Unloading | releasing CPU/GPU/cancellation |
| Failed | error; retry/fallback/diagnostic |
| Disposed | memory released, handle invalid |

## 3. Policies

| Policy | Behavior |
|---|---|
| `Lazy` (default) | load on first request/visible/near viewport |
| `Preload` | queue ahead of need with priority/deadline |
| `Pinned` | never evict/unload while pinned (critical screen) |
| `Streaming` | progressive chunks/frames; partial ready state |
| `CacheOnly` | use cache; no network; fail/placeholder if missing |
| `EagerCritical` | explicitly in the startup critical set (small only) |

## 4. API

```cpp
enum class ResourceClass {
    Bootstrap, Font, Icon, Image, Route, Data, Media, ShaderPipeline, User
};

enum class ResourceState {
    Declared, Queued, Loading, Ready, Active, Idle, Evictable, Unloading, Failed, Disposed
};

struct ResourceId {
    std::uint64_t value;
};

enum class ResourcePolicy { Lazy, Preload, Pinned, Streaming, CacheOnly, EagerCritical };

struct RequestOptions {
    ResourcePolicy policy = ResourcePolicy::Lazy;
    int priority = 0;                 // higher = sooner
    std::int64_t deadline_ns = 0;     // optional
    bool allow_network = true;
    bool placeholder = true;
};

class ResourceHandle {
public:
    ResourceHandle() = default;
    ~ResourceHandle();                // releases the lease
    ResourceHandle(ResourceHandle&&) noexcept;
    ResourceHandle& operator=(ResourceHandle&&) noexcept;
    ResourceHandle(const ResourceHandle&) = delete;
    [[nodiscard]] bool valid() const;
    [[nodiscard]] ResourceState state() const;
};

class ResourceManager {
public:
    Result<ResourceHandle> request(ResourceId id, RequestOptions options = {});
    Status preload(ResourceId id, RequestOptions options = {});
    Status pin(ResourceId id);
    Status unpin(ResourceId id);
    Status unload(ResourceId id);      // fails if pinned/active
    void set_budget(ResourceClass cls, std::uint64_t bytes);
    std::uint64_t memory_used(ResourceClass cls) const;
};
```

- `request` deduplicates in-flight loads and returns a lease when ready.
- `ResourceHandle` is RAII; copy is disabled to make ownership explicit (use explicit clone if
  needed).
- `preload` does not create a user-visible lease; it warms the cache.
- `unload` is explicit; eviction calls it when no leases/pins.
- Failed requests return an error and may leave a placeholder.

## 5. Loading pipeline

```text
request -> cache lookup (ready) -> dedup in-flight -> queue
  -> async load/decode (worker/IO)
  -> GPU upload (resource pipeline)
  -> ready -> completion queue -> UI thread applies
  -> lease active
```

- Cache hit: no I/O; mark recently used.
- In-flight dedup: one load per id; multiple waiters share the result.
- Cancellation: lease release before ready cancels if no other waiters.
- GPU upload is batched/pooled; no per-resource submit.
- Completion is delivered at a frame boundary; no UI mutation from worker threads.

## 6. Preload

Triggers:

- route intent (hover/focus/press, back-stack restore)
- viewport proximity / virtual list overscan
- post-auth shell (preload user home)
- idle time / warm start
- next media item/page
- explicit developer preload list

Rules:

- Preload has priority/deadline and is cancellable.
- Preload respects network/power policy (no aggressive prefetch on battery/power-save).
- Preload is bounded per tick/frame and by queue depth.
- A preloaded resource still needs a lease when used; preload does not pin forever.
- Preload is not a reason to block first frame.

## 7. Unload and eviction

Triggers:

- last lease released -> Idle
- route exit -> unload route-scoped resources
- app background/minimized (policy)
- logout -> unload user-scoped resources, clear caches
- memory pressure -> evict Idle/Evictable, largest/lowest-priority first
- cache budget exceeded -> LRU/age eviction
- hot reload/asset generation change -> old generation retires

Rules:

- Never unload Active/Pinned resources.
- GPU resources retire after in-flight frames complete.
- Unload cancels pending loads and drops stale completions.
- Eviction must not change current-frame pixels.
- Unload/reload metrics reveal thrashing; a resource that reloads every frame is pinned or
  budgeted wrong.

## 8. Budgets

- Per-class and total budgets (bytes/items).
- Startup critical set is small and explicit.
- Route budgets: route code/data/images/media.
- Cache budgets: texture/glyph/path/draw-list/layer.
- Preload budget per tick/per frame and network bytes.
- User tunables may adjust some budgets/caps, but only within hard limits
  ([`buffer-config-v1.md`](buffer-config-v1.md)).

## 9. Determinism

- Golden mode pins the critical set and full load order; no network/system fonts.
- Async completion order must not change output; apply in deterministic order.
- Resource ids/keys are stable; no random ids.
- Unload/eviction in golden mode is disabled or fixed.

## 10. Errors and fallback

- Missing asset: deterministic placeholder + diagnostic.
- Network failure: cache-first/offline; retry/backoff; no retry storm.
- Decode failure: error placeholder; never crash.
- OOM: evict caches, keep previous generation, report; no crash.
- Auth/secure resources: fail closed; never show stale user data.
- Cancellation is not an error unless the caller treats it as one.

## 11. Metrics and tests

Metrics:

- state counts, queue depth, in-flight loads, wait time
- cache hit/miss/eviction, memory by class, peak
- preload hits/misses, cancellation, thrash/reload count
- unload bytes/time, GPU retirement backlog
- startup/route critical-set load time

Tests:

- state transitions, duplicate requests/dedup, cancellation
- lease/refcount: unload only when idle; pinned/active protected
- eviction: LRU/age, memory pressure, no current-frame eviction
- preload: priority/deadline/cancel, no blocking first frame
- offline/error/placeholder/retry
- old generation retirement after frames in flight
- deterministic load order/golden output
- long-run route churn: no leak, no thrash

## 12. Open questions

- Cache-first vs network-first defaults per resource class.
- Disk cache format/encryption/eviction.
- Streaming protocol for large media/data.
- Preload budgets on mobile/power-save.
- Whether preload should create a weak lease.
- Per-route resource scoping/ownership.
- Cross-process/shared cache for multiple windows.
