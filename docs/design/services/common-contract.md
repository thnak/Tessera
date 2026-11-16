# Common contract

Part of the [services design set](README.md). Section 1 is the contract every service follows;
1.1 is the shape that landed with AF1 (ADR-050 section 4).

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
- Results are delivered on the UI thread through the completion queue ([`async.md`](../async.md)).
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
consequence of the existing coroutine runtime (see [`async.md`](../async.md)), not a change to the
service contract.

What AF1 implements, and what it deliberately leaves:

| ADR-050 section 4 item | AF1 |
|---|---|
| 1 — topological start, reverse stop | yes; a cycle or a missing id fails `prepare()` |
| 2 — start phases (`Runtime`/`Background`/`Idle`), `required` gating | no: `required` is recorded and its failure reported through `required_failure()`, which AF3 polls; phase ordering is AF3 |
| 3 — one `TaskScope` per service | not in AF1; the type landed in AF2 (issue #174) as `appfw::TaskScope`, and per-service owning is the service runner's wiring in a later slice. See [`background-work.md`](../background-work.md) |
| 4 — bounded non-blocking shutdown | yes for stop (500 ms per service inside a 2 s total). Start ships **without** a deadline: the ADR specifies no start budget, and the mechanism is implemented and tested so a future ADR line can enable it |
| 5 — errors are values, exceptions caught at the boundary | yes: an escaped exception becomes `Failed`, never reaches the frame loop |
| 6 — virtual dispatch at the service boundary | yes |

Two readings of underspecified transitions, both conservative and both stated in the header:

- **Only a service that reached `Started` is stopped.** A service whose start failed cleans up after
  itself, because ADR-050's diagram defines no `stop()` for a service that never ran.
- **A stop that throws is `Abandoned`, not `Stopped`.** The diagram gives `Stopping` only those two
  outcomes, and an unconfirmed stop is not a confirmed one.
