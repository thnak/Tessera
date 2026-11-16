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
| 3 — one `TaskScope` per service | not in AF1; the type landed in AF2 (issue #174) as `appfw::TaskScope`, and per-service owning is the service runner's wiring in a later slice. See [`background-work.md`](background-work.md) |
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

### 2.1 Implemented shape (AF8, issue #195)

The `i18n` module ships as `tessera_appfw_i18n` (ADR-050 section 1 layer 1) in four pieces:
`LocaleTable` (negotiation), `MessageCatalog` (the compiled table), `LocaleService` (the runtime
state and its change signal) and `LocalizedText` (what a locale change dirties). It is the smallest
slice that satisfies the issue's acceptance - *"locale change updates text without relayout bugs"* -
and the rest of [`i18n-v1.md`](../specs/i18n-v1.md) is named below rather than half-built.

**The supported set is compiled-in data, not a probe of the machine.** `LocaleTable` is built from
`{tag, direction}` pairs the app ships, and nothing in the module reads an OS locale, calls
`setlocale`, or links ICU. That is i18n-v1.md section 8.1's "ICU data is ... pinned; locale packs are
bundled" read as the *only* mode rather than a test mode: the same requests resolve identically on
every machine, which is also what the acceptance's "tests are deterministic" requires. An ICU-backed
table fills the same `SupportedLocale::direction` field later, so wiring the reduced data pack
(`TESSERA_ICU_REDUCED_DATA`, off in the default build and CI) is a data change, not an API change.

**The requests are an ordered list, because section 1 names four inputs.** Section 8.1 writes the
chain as a single `requested -> normalize -> exact -> language+script -> language -> fallback ->
default`, so the list is how "OS locale(s), app default, user override, per-window override" arrives
without an OS call: the composition root passes them in priority order, the table walks the chain
once per request, and the first request that resolves wins. `requested` in the result names the
request that served the locale, not the first one tried, so a diagnostic always explains `resolved`.
Normalization is the reduced canonical form `src/ui/text/locale_resolver.cpp` already documents
(`-` to `_`, language lowercased, a two-letter region uppercased, a four-letter script title-cased),
so the tree has one spelling of a tag rather than two.

**The epoch is what makes a redundant request free.** `LocaleService` publishes the resolved locale
through AF4's `Observable<LocaleSnapshot>`, whose `operator==` is "same tag and same direction" - the
epoch is deliberately *not* part of equality. `set_requests` with the same request therefore stores
nothing, advances no epoch and wakes no ticket, so a settings panel that forwards every keystroke
cannot relayout on every key. A real change increments `locale_epoch` (section 8.1) and delivers one
notification per flush, which is what invalidates a formatting cache or re-renders a label.

**A locale change is classified by measurement, not by a constant.** This is the dirty-rendering
half of the acceptance, and it is `LocalizedText`'s whole job:

| Case | Mask | Why |
|---|---|---|
| wider/narrower translation, or one that wraps | `Layout \| Paint \| Render` | the "text/font change" row of [`dirty-rendering.md`](dirty-rendering.md) section 3. Filed under Paint only, the new string would paint at the old box and clip |
| same-width re-translation | `Paint \| Render` | no box moved; relayouting for it is the wasted work the granularity exists to avoid |
| direction flip | `Style \| Layout \| Paint \| Render` | the resolved base direction seeds the inherited `direction` property (section 4), so the style *input* changed and every logical inline-axis property downstream moved |
| same locale again | `Clean` | a re-published locale must not relayout |

Measurement goes through the `text::TextMeasureCache` layout itself uses, so the reported box is the
box Yoga will produce; the `appfw_i18n` test asserts the masks (`appfw_i18n_relayout_tests.cpp`) *and*
the measured size through the real `LayoutEngine` and `LayoutScheduler`
(`appfw_i18n_layout_tests.cpp`), for the English and German strings. The tests use
`StubTextMeasurer` with an explicit metrics provider, so an advance is a fixed multiple of the code
point count and the assertions are exact rather than tolerance-dependent.

**What this slice deliberately does not do**, and what each piece needs (the next slice, not a
guess):

- **ICU MessageFormat** (`{count, plural, ...}`, `select`, nesting) and number/date/currency/
  relative-time formatting (sections 2 and 3). A pattern is returned verbatim today, so a plural
  string would be shown literally. This needs the ICU data pack in the build; `MessagePattern` is the
  seam a compiled AST replaces without touching a caller.
- **Locale-aware parsers** for number/date input (section 5) and **accessibility announcements** in
  the new locale, which needs the a11y module's announcement queue.
- **The `en-XA`/`ar-XB` pseudo-locales** (section 8.1). Both are build artifacts that the reduced
  ICU data step generates; the *layout* half of `ar-XB` is already covered by `logical_layout`, which
  lays one stylesheet out under both `direction` values.
- **Persisting the user override** through AF7's settings store, and the **OS-locale source**
  itself: a host passes its platform locales as the last requests in the list. Reading them is a HAL
  concern with its own platform lanes, and putting it here would have made the module's tests depend
  on the machine by construction.
- **Catalog build/validation** (section 6: ICU syntax validation, unused-key CI failure). The runtime
  half is enforced - a duplicate `(id, locale)`, an unsupported locale, or an id missing from the
  default locale is an `I18nError`, not a silent no-op - and the generator that emits
  `MessageEntry[]` is the build slice.

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

### 4.1 Implemented shape (AF7, issue #179)

The `storage` module ships as `tessera_appfw_storage` (ADR-050 section 1 layer 1): documents and
settings, every byte through an I/O executor, every write an atomic replace, and a corrupted entry
reported rather than returned. This subsection is the design record for what landed and for the three
decisions the ADR left open.

**Where `IoExecutor` lives: `runtime`, as a seam plus two implementations.** Layer 0 already owned
the *mechanisms* - the bounded `WorkerExecutor` pool, the bounded in-flight `IoExecutor` front end,
and the `CompletionQueue` the UI thread drains - but nothing that a module could be *handed* without
naming a thread. AF7 adds that interface as `runtime::IoTaskExecutor`
(`include/tessera/runtime/io_task_executor.hpp`) plus two implementations: `WorkerIoExecutor` (one
bounded pool, completion posted to the shared queue) for production, and `ManualIoExecutor` (two
hand-drained FIFOs, no thread, no sleep) for tests. It is in layer 0 for ADR-050 section 1 rule 1's
reason: an abstract executor is a seam, and putting it in the module would force every other module
to depend on storage. `post()` never runs the work inline, so a UI-thread caller cannot accidentally
perform the blocking call, and the completion is always delivered through the queue.

**The atomic-write sequence, and why each step is where it is.** `appfw::atomic_replace`
(`src/appfw/storage_atomic.cpp`) is the only code path that writes:

| Step | Call | Why it is that step |
|---|---|---|
| 1 | create the parent directory | an absent root is a first-run condition, not an error |
| 2 | write the complete payload to `<target>.<generation>.tmp` | the target is never opened for writing, so there is nothing to tear |
| 3 | flush the temp file | the rename must not make bytes visible that are still in the page cache |
| 4 | replace the target with the temp file | the only step a reader can observe |
| 5 | `Durable` only: flush the directory | on POSIX the new directory entry is the part not on disk yet |

The temp file is created **in the target's own directory**, because a rename across devices is a copy
and a copy is not atomic. That is the one invariant a caller cannot verify from outside, so it is
asserted (`temp_path_for` is exposed and tested).

Step 4 is a platform primitive, not `std::filesystem::rename`. Windows is the lane where the portable
call is wrong twice over: `std::rename` fails with `EEXIST` when the target exists, and
`std::filesystem::rename` is not specified to replace at all. `hal::FileSystem::replace` is
`MoveFileEx(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows and `rename(2)` on POSIX
(`src/platform/{windows/filesystem_win32,linux/filesystem_posix}.cpp`), and the real-file test runs
the sequence against the actual OS rather than trusting the comment.

**Which `Durability` is the default, and the fsync policy per platform.** This is the deliberate
deviation from the bullet above. Both policies flush the temp file; only `Durable` flushes the
directory, and `BestEffort` is the default. The failure window `Durable` closes is narrow: a *crash*
may lose the last write, but a reader still sees the complete old content or the complete new
content, never a mixture - because the rename is atomic either way. Paying a directory flush per
write for a UI-facing settings store is not a trade this slice makes silently, so it is opt-in and
per-`StorageConfig`.

| Platform | `Durable` step 5 | Note |
|---|---|---|
| Windows | `MoveFileEx` already flushed the rename; `platform_flush_directory` is a successful no-op | Win32 has no supported directory fsync |
| Linux | `fsync` on an `O_DIRECTORY` descriptor (`ENOTSUP` -> `Unsupported`) | without it a power loss can leave the bytes on disk under the old name |

**Corruption fallback.** An entry is not raw payload: a 24-byte header carries a magic, the format
version, the entry *kind* (document vs settings), the payload length and an FNV-1a checksum
(`src/appfw/storage_envelope.cpp`). Each field has its own diagnostic step, so "header-short" (a
torn write), "header-magic" (a foreign file), "header-length" (a truncation) and "checksum" (a bit
flip) are distinguishable. Every one reports `Corrupted`, records a bounded diagnostic, and **leaves
the file in place**: a corrupt read never deletes data behind the user's back. The caller's fallback
is therefore its own default, which is what the acceptance criterion asks for.

**Cancellation.** Every operation takes a `CancellationToken`. An already-cancelled token is refused
at submit (the same rule `runtime::IoExecutor::submit` uses), so nothing is posted. A cancel that
arrives while the body runs is observed between whole steps; the body then removes its temp file and
the target keeps its old content. `StorageDiagnostics` counts `aborted_writes` and
`temp_files_removed` separately, because "the write was abandoned" and "the temp file is gone" are
two claims and the test asserts both.

**What this slice does not do.** No migrations (the format version is checked, not upgraded), no
cache class or quota eviction, no keystore/encryption, and no streaming for large documents (a write
is one bounded buffer, capped by `StorageConfig::max_entry_bytes`). Each of those is a later slice of
the same module; the header names them so a caller does not assume they exist.


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
- ~~Who marks the in-frame service-lookup region.~~ **Decided by AF3 (issue #175); see section 11.**
  ADR-050 section 3 item 5 forbids resolving a service while a frame is being updated and defines a
  counter for it, but the region has to be marked by somebody. AF1 (issue #173) ships the marker as
  `appfw::FrameScope` and the counters as `service_lookup_audit()`, and deliberately does **not**
  wire it into `runtime::RuntimeLoop`: layer 0 gains seams, not concepts (ADR-050 section 1 rule 1),
  and a whole-tick mark would count a service's own legal `start()` lookup, because a start resumes
  from the completion drain that runs inside the tick.

## 11. App lifecycle and the AF3 rulings

AF3 (issue #175) implements ADR-050 section 2: `appfw::Application`, the seven-state machine, ordered
lifecycle dispatch to services, the termination gate with `Defer`, and the `WindowHost` seam. Two
questions AF1 deferred are decided here, because the answers are properties of this slice.

### 11.1 Who marks the in-frame lookup region: the appfw tick wrapper

**Decision: `appfw::Application`'s tick wrapper marks the update/render stage; `RuntimeLoop` is left
untouched.** `RuntimeLoop::tick()` drains the completion queue first and only then advances the fixed
step and reports what to render (see `src/runtime/runtime_loop.cpp`), so the drain - the one place a
service's legal `start()`/`stop()` lookup can resume - is inside the same call as the update. A
`FrameScope` spanning the whole tick therefore marks the drain too and would count a service's own
`start()` resolution as a violation, which is a false positive on a contract the ADR explicitly
permits. Marking only the stage after the drain is not expressible from outside `RuntimeLoop::tick()`,
and splitting the method to expose the seam would put an app-framework concept (a lookup region) into
layer 0, which ADR-050 section 1 rule 1 forbids.

The wrapper is the app-framework tick boundary: the host that owns the frame step opens a
`FrameScope` around its update/render work (`begin_frame` through `end_frame`), never around
`RuntimeLoop::tick()` and never around the completion drain. `FrameScope` already counts nesting, so
a caller that wraps its own subsystem does not cancel the outer mark. The audit counter is therefore
authoritative for the update/render window the ADR names, and the drain stays legal.

**What AF3 actually wires, stated precisely.** AF3 records the decision and provides the machinery;
it does not yet open a `FrameScope` inside `WindowHost`. Nothing resolves a service while a frame runs
today: component `mount()` lookups arrive with AF5's codegen, and AF3's own service callbacks are
lifecycle notifications, not frame work. Wiring a mark with no caller would be dead code that the
tests cannot distinguish from a working one, so the mark is placed by the host slice that first has
in-frame lookups to catch, and `docs/reference/open-questions.md` records this entry as resolved with
that scope. What AF3 does guarantee is the other half: the dispatch path (`Application::notify_observers`)
runs outside any frame region, so a service reading a cached pointer in `on_app_state` can never be
miscounted as a violation.

Consequence for the acceptance evidence: the zero-violation claim is scoped to "no lookup while a
`FrameScope` is open", which is what AF1's tests assert. A lookup made from `on_app_state` is *outside*
that region (dispatch is not update/render work), so AF3 does not mark dispatch either: the ADR's rule
is about the frame path, not the lifecycle callback.

### 11.2 Does `start()` get a deadline: no

**Decision: `start()` keeps no deadline in V1.** ADR-050 section 4 item 4 pins a budget for shutdown
only (500 ms per service inside 2 s total), and says nothing about start. `ServiceRunnerConfig` names
the start deadlines separately (`per_service_start_deadline_seconds`,
`total_start_deadline_seconds`) with `0` meaning disabled, so the mechanism exists and is tested;
enabling it for production needs a line in ADR-050 section 4 item 4 that states the two numbers and
what a start that overruns should become (`Failed` today, by analogy with a failed start, but the ADR
does not say). Deciding that here would invent a startup budget for every app in the repository.

What AF3 does instead, so a start that never finishes cannot hang the lifecycle forever: `launch()`
and `tick()` drive `ServiceRunner::tick()` (which enforces whatever deadlines are configured), and the
app stays `Launching` until the start sequence leaves `Starting`. A host that wants a bound configures
the two fields; the framework does not silently pick one.

### 11.3 The ordered dispatch, and how existing signals route into it

The lifecycle is not a second mechanism beside the driver's occlusion pause; it is a layer above it.
`WindowHost` already folds normalized `hal::WindowEvent` values into `lifecycle_paused_` /
`metrics_paused_` (src/app/window_lifecycle.cpp) and honors `WindowPausePolicy`. AF3 adds one borrowed
`app::LifecycleSink*` to that fold (src/app/window_lifecycle_seam.cpp) and forwards the transitions it
already sees:

| Driver source | Forwarded as | Reaches |
|---|---|---|
| `WindowEventType::Focus` / `Blur` | `focus` / `blur` | `Application::dispatch` |
| `WindowEventType::Minimized` / `Occluded` | `minimize` / `occlude` | `Application::dispatch` |
| `WindowEventType::Restored` (with the live metrics) | `restore` | `apply_window_snapshot` |
| `WindowEventType::Close` | `close` | `dispatch` -> termination gate |
| `WindowMetrics` `occluded`/`minimized`/`focused` moving | the matching event, once per transition | `dispatch` |

Pause behaviour is unchanged: focus/blur never pause (a visible background window keeps painting), the
resume edge still latches a full repaint, and a window with no sink installed behaves exactly as
before. `idle_policy.hpp`'s pacing values are not lifecycle input and are not routed: they schedule
frames, they do not change app state.

Order is the ADR's, and it is what the tests assert as a vector:

1. **Forward transitions** (launch to `Active`/`Inactive`/`Hidden`, and `Resume`) dispatch in the
   runner's start order, so a dependency observes the new state before the dependents that read it.
2. **Graceful-stop transitions** (`Suspended`, and `Terminating`) dispatch in reverse start order,
   mirroring ADR-050 section 4 item 1's reverse `stop()`, so a dependent suspends before the
   dependency it uses.
3. A service observes transition *N* completely before any service observes *N+1*: dispatch is
   synchronous and one transition per call, never interleaved.
4. Only a service that reached `Started` is told; one whose start failed has nothing to react to,
   which also matches AF1's rule that only a `Started` service is stopped.

The termination gate runs the same reverse order: `on_terminate_requested()` is asked in reverse start
order, one `Defer` aborts the sequence and leaves everything running (`deferring()` reports it), and an
all-`Allow` sequence enters `Terminating`, notifies in reverse order, and begins AF1's bounded reverse
stop. An exception from a lifecycle callback is caught at the boundary (ADR-050 section 4 item 5): the
transition stands and the remaining observers are still called.

### 11.4 Background work: `TaskScope` and the timer wake source (AF2, issue #174)

The roadmap row pairs two things and they land in different layers, which is the first thing to know
about this slice:

| Piece | Layer | Where | Why |
|---|---|---|---|
| `appfw::TaskScope` | 1 | `include/tessera/appfw/task_scope.hpp` | it is an app-framework concept: bounded children of a service, window or component |
| `runtime::TimerQueue` | 0 | `include/tessera/runtime/timer_queue.hpp` | a timer is a wake source the loop schedules against, not an app concept |

`TaskScope` guarantees, in the ADR's own order: `spawn()` is the only start path, so no detached task
is expressible; the child table is fixed-capacity and a full scope refuses instead of growing;
`cancel()` flips one token that every child checks before its next resume; `close()` records a
deadline and returns, and `tick()` reports `Open`/`Closing`/`Closed`/`TimedOut`, so shutdown is
tick-driven and never blocks the UI thread. Scopes nest through the parent's `CancellationToken`,
with the registration held weakly so an outlived child scope is never written through.

**Cancellation while a child is suspended** is the case that decides the shape, and the rule is that
a suspended coroutine frame is never destroyed by the scope. Each child runs under a token-free
driver coroutine that holds the scope's bookkeeping weakly; the frames are owned by the task graph,
so destroying the `TaskScope` cancels and drops the handle without dangling. A child that refuses to
finish leaves a bounded, visible cycle (`live()` stays non-zero and `tick()` reports `TimedOut`)
rather than being destroyed under an awaitable that may still hold its handle. The reason a driver's
own context must be token-free is the same split: a tokened driver would be skipped exactly when its
child is cancelled, and the driver is what retires the slot. AF1 records the identical reasoning for
service attempts in `src/appfw/service_bridge.hpp`.

The timer side is a layer-0 seam in the strict sense of ADR-050 section 1 rule 1: nothing in
`timer_queue.hpp` or `runtime_loop.hpp` names `appfw`, a service or a store. `RuntimeLoop::tick()`
fires due timers after the completion drain and before the fixed step, and never turns a fire into a
frame request; `next_timer_wait_seconds()` is the pure query a host joins with the pacer deadline, so
an idle app parks until the earlier of the two. Firing order is `(deadline, registration sequence)`,
which is what makes "timers fire in order" deterministic for equal deadlines, and a periodic timer
that missed several periods fires once and advances past `now` instead of bursting.

The acceptance evidence is counters, never elapsed time: with only a far timer pending, a
`ManualClock`-driven loop is idle at `t = 0` with `next_timer_wait_seconds() == 10.0`, presents zero
frames, and costs exactly one further tick after the clock reaches the deadline. Details, the
re-entrancy rules and what the slice deliberately does not wire (the `WindowHost` park join and
`Application`'s root scope) are in [`background-work.md`](background-work.md).

### 11.5 `useService<T>()`: what codegen emits (AF5, issue #177)

AF5 lands the IR-to-codegen half of ADR-050 section 3.1. The TSX half is a separate slice; section
11.6 states exactly what it needs and what this slice deliberately does not invent.

**The TSX contract becomes IR data, not a codegen convention.** `ir::Component::services` is an
ordered list of `{id, name, type, header, source}` entries. `type` and `header` are the C++ spelling
of the interface (`demo::Navigator`, `demo/services.hpp`), which the project's `ui/services.json`
manifest supplies (ADR-050 section 3.1 item 1); the IR carries them as data because codegen reads
the IR only (AGENTS.md non-negotiable 4). The field is additive and omitted from the IR JSON when
empty, so a module without `useService` is byte-identical to the IR before it existed. This is
deliberately *not* `ir::Environment`: an environment value is read through a provider on every use,
a service is a typed pointer cached at mount.

**The generated base**, for a component with N requirements:

```cpp
std::tuple<demo::Navigator*, demo::MailStore*> services_ = {};   // private, once
void attach_services(const tessera::appfw::ServiceContext& context);  // N lookups, once
void mount(const tessera::appfw::ServiceContext& context);            // attach, then mount()
template <class T> [[nodiscard]] T& service() const;                  // cached pointer
[[nodiscard]] demo::Navigator& nav() const;                           // one per requirement
```

Two decisions are worth naming. **The context is not retained**: after `attach_services()` the only
state is the typed pointers, so "no resolve in the frame path" (ADR-050 section 3 item 5) is
structural rather than a review rule -- there is no registry left to look up from. And **the
diagnostics live in `service<T>()`, not in `build()`**: they are properties of one call, not of the
component's lifetime, so a component that declares a requirement but never reads it still compiles.

| Code | When | Message shape |
|---|---|---|
| `TESSERA_CG_SERVICE_UNDECLARED` | `service<T>()` for a valid interface the component never declared | names the component and every declared requirement with its TSX `file:line:column` |
| `TESSERA_CG_SERVICE_NOT_INTERFACE` | `service<T>()` for a type with no `kServiceId` (ADR-050 section 3 item 2: never a concrete type or a string key) | names the component and states the `kServiceId` requirement |

Both are `static_assert`s carrying the code, the same convention as `TESSERA_CG_MISSING_BINDING`
(`emit_base.cpp`). They are instantiated by a nested `ServiceRequirement<T>` struct, so the
compiler's own instantiation trace names the offending type next to the message: a developer sees
`ServiceRequirement<demo::Settings>` and `service<demo::Settings>` at their call site, not a bare
template dump. A type that is not an interface fails only the interface assert, so one mistake gives
one diagnostic instead of a second, misleading "not declared" for the same name.

**Emission is conditional on the component having a requirement.** A component that does not call
`useService` generates not one byte of service code -- no include, no slot, no accessor -- which is
why every pre-existing `.g.hpp` golden stays byte-identical (`git status tests/golden` shows only
the new `tests/golden/codegen/services/` directory).

**The resolve cost is checked twice, and neither check is a wall-clock gate.** The deterministic gate
is `codegen_services_runtime`: with AF1's `service_lookup_audit()` counters, 1 000 accessor reads
inside a `FrameScope` add **zero** lookups and zero misses, and the same holds for a read from a
generated event handler -- paired with value checks (the provided instance's state is observed
through the accessor), so an accessor that did nothing could not pass. The documented measurement is
[`service-resolve-benchmark.md`](service-resolve-benchmark.md) section 4.5: an accessor read is
0.37 ns, the same machine code as the cached-pointer case, while resolving from a retained context on
every use costs 4.61 ns, about 12x even with the registry entirely in cache.

### 11.6 The TSX half of `useService<T>()`: what is still missing

Not in this slice, and not guessable from the ADR:

1. **Call-expression lowering.** A service use is `nav.push(route)`, and `lower_reference` gives
   member chains an IR form only for a prop object, a record-typed state and a loop item
   (`reference_lowering.cpp`); a call on that chain has no IR expression today, and
   `TESSERA_JSX_SUBSET_CALL` rejects it at the front end. `useService` is therefore only useful once
   a member call on a service value is an IR expression.
2. **A hook form.** `useService<T>()` needs an entry in `rules` (`check_hook_statement`), a
   `SymbolKind`, and a `lower_hooks` branch that appends to `ir::Component::services`. Its `T` is a
   type argument the front end currently only parses for `useState`/`useRef`.
3. **The manifest and its typings.** `ui/services.json` (id, C++ class, header, the methods TSX may
   call with their IR types) plus the generated `services.d.ts` is a spec-level artifact with no
   format in the repository yet; ADR-050 section 3.1 item 1 names what it must contain but not its
   schema. Writing one here would be an ADR/spec decision, so the IR entry took `type` and `header`
   as data and left the reader to that slice.
4. **Startup validation of missing providers**, which needs the manifest's union across components to
   know what to require (ADR-050 section 3.1 item 6). AF1's runtime form of the same failure already
   terminates with `TESSERA_APPFW_SERVICE_MISSING`.

