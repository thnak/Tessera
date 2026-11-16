# Service lifecycle

Part of the [ADR-050 decision set](README.md). Section bodies here: `### 4.`. The heading
numbers, levels and order are the original ones, so `ADR-050 section N` still names the same
heading. Each part is a verbatim slice of the former single file; the shared front matter
(status, date, related decisions, terms) is in the [index](README.md).

### 4. Service lifecycle

```cpp
class Service {                       // virtual dispatch: tens of objects, rare calls (measured, appendix)
public:
    virtual ~Service() = default;
    virtual runtime::Task<Status> start(StartContext& ctx) = 0;   // ctx: scope, token, deps
    virtual runtime::Task<void> stop(StopContext& ctx) = 0;       // bounded by ctx.deadline
    virtual void on_app_state(AppState state) {}                  // UI thread, must not block
    virtual TerminateVote on_terminate_requested() { return TerminateVote::Allow; }
};
```

```text
Registered -> Starting -> Started -> Stopping -> Stopped
                  \           \          \
                   +-> Failed  +-> Failed +-> Abandoned (stop deadline expired)
```

1. **Order.** A service declares `kDependsOn` (a list of service ids). `Application` sorts services
   topologically at freeze time; a cycle or a dependency on an unregistered id is a startup error.
   Stop runs in reverse order.
2. **Phases.** Each service declares `StartPhase::Runtime` (P1, cheap, needed for the first
   frame, for example the logger sink), `StartPhase::Background` (P3, the default) or
   `StartPhase::Idle` (P5). A service marked `required` gates P4; an optional one does not.
3. **Ownership of work.** Each service gets its own `TaskScope` (section 5). Everything a service
   starts belongs to that scope; when the service reaches `Stopped`, its scope is cancelled and
   drained, and completions tagged with its token are dropped instead of running.
4. **Bounded, non-blocking shutdown.** `Terminating` runs a state machine pumped by `RuntimeLoop`
   ticks: stop services in reverse order, each with a per-service budget (default 500 ms) inside a
   total budget (default 2 s, a `runtime-config-v1.md` field). A service past its deadline is
   cancelled and marked `Abandoned` with a diagnostic; shutdown continues. Nothing calls `join()`
   or waits on the UI thread; worker pools are shut down last under their own bounded drain.
5. **Errors.** `start` returns a `Status` value. An exception escaping `start`, `stop` or a
   callback is caught at the service boundary (ADR-025 rule: exceptions at boundaries), turned
   into `Failed` plus a diagnostic, and never propagates into the frame loop. A failed required
   service routes to the root error boundary (same path as `TESSERA_STARTUP_CRITICAL_FAILED`); a
   failed optional service leaves `find<T>()` returning null.
6. **Dispatch.** The lifecycle base and service interfaces use `virtual` functions; this departs
   from ADR-040's CRTP style only at the service boundary. A concrete service may expose
   non-virtual, inlinable methods to C++ callers that name it directly. Any service method called
   per frame (rare; for example a value read by an animation) must be reached through a cached
   pointer to a `final` type so it can inline.
**Implementation note (AF1, issue #173).** The signature above is the contract, but its body cannot
literally be a coroutine with the in-tree `runtime::Task`; AF1 keeps the signature and moves the
coroutine to a context-first free body. Mechanics and the AF1/AF3 split: [`services/common-contract.md`](../../docs/design/services/common-contract.md#1-common-contract) section 1.1.
