# Networking module: a `fetch`-shaped seam with named outcomes (issue #197)

Status: implemented (first slice). The normative contract is
[`docs/specs/networking-v1.md`](../specs/networking-v1.md) and [ADR-052](../../decisions/ADR-052-networking-platform-stacks.md);
the behavioural record for the module's shared contract is
[`services/platform-modules.md`](services/platform-modules.md) section 5 and [`services/common-contract.md`](services/common-contract.md) section 1. This note is the longer rationale, the
spec/code disagreements, the state machine and the test map. Code:
`include/tessera/appfw/net_{types,url,redirect,state,transport,fake,client}.hpp`,
`src/appfw/net_*.cpp`.

## 1. What landed, and what did not

Section 4 of the spec orders six back ends. **One landed: `FakeNet`.** The other five need
`docs/specs/dependencies.md` and `THIRD_PARTY_NOTICES.md` entries before their slice can start
(ADR-052 item 5 says so for libcurl/TLS explicitly), and issue #197 itself says "one PR per back
end". What this slice lands is everything *above* a back end - the part every one of the six would
otherwise duplicate and get subtly different:

| Landed | Where |
|---|---|
| the interface of section 2 (`NetRequest`, `NetResponse`, `NetError`, the eleven kinds, the defaults) | `net_types.hpp` |
| URL parsing, origin comparison, diagnostic redaction (section 3 items 2, 8, 9) | `net_url.hpp/.cpp` |
| the redirect rules (section 3 item 2), once, for every back end | `net_redirect.hpp/.cpp` |
| the per-request state machine and the outcome vocabulary | `net_state.hpp/.cpp` |
| the transport seam (one abstract class, no platform type) | `net_transport.hpp` |
| `FakeNet`, the deterministic back end section 4 orders first | `net_fake.hpp/.cpp` |
| `WinHttpTransport`, the Windows back end (#835), built on Windows only | `net_winhttp.hpp`, `windows/net_winhttp.cpp`, `net_winhttp_map.*` |
| the client: `send`, `submit`, cancellation, the deadline, delivery | `net_client.hpp`, `net_client*.cpp` |
| the contract suite of section 5 | `tests/unit/appfw_net_*` |
| the same contract against WinHTTP over loopback (opt-in `TESSERA_NET_LOOPBACK_TESTS`) | `tests/unit/appfw_net_winhttp_tests.cpp` |

**WinHTTP (#835).** One async session per transport (`WINHTTP_FLAG_ASYNC`, status callback), so the OS
stack owns connections, pooling, TLS, HTTP/2 and the system proxy (`AUTOMATIC_PROXY`); the framework
adds none of those and exposes no switch for them. Decisions: redirects are never followed by WinHTTP
(`REDIRECT_POLICY_NEVER`), so `NetClient` is the one follower and the cross-origin `Authorization` rule
holds; cookies and WinHTTP's credential handling are disabled, so the request's headers are the
caller's; the agent string is empty, so `User-Agent` is the module's; `max_response_bytes` stops the
read and the request is closed. A cancelled token reaches WinHTTP through a closer thread the transport
owns (a token callback runs on the cancelling thread and must not call into WinHTTP), and the
destructor cancels what is in flight and waits for every handle. **Not distinguished yet:** `offline`
(WinHTTP reports a lost connection as a connect failure, so it surfaces as `connect`); an `offline`
signal needs the network-list API and is a follow-up.

Not landed, each with the reason: **libcurl**, **browser `fetch`**, **NSURLSession**
and **Android** (one PR per back end, and the Linux one needs dependency entries first); **TLS**
(negotiated inside a back end, deliberately not owned by the framework - ADR-052 item 5);
**HTTP/2 and HTTP/3** (the platform stack picks the version; `NetProtocol` only reports it, and
section 7 leaves HTTP/3 to the platform); **WebSocket and server-sent events** (section 1
non-goals: a separate interface, after V1); **streamed response bodies** (`Request.stream` appears
in the `Response` row of section 2's table and nowhere in its `Request` row, so the field does not
exist; section 7's "response streaming to disk" is the same gap); **certificate pinning**, a cookie
jar and an HTTP cache (section 1 non-goals); **relative redirect resolution** (section 7 below).

## 2. Where the spec and the code disagreed, and which won

Three places. Each is a gap in the spec rather than a preference, and in all three the spec's
intent won and the deviation is a named value instead of an implicit one.

**1. `send` cannot return the spec's type.** Section 2 writes
`send(Request, CancelToken) -> Task<std::expected<Response, NetError>>`. `runtime::Task`'s promise
takes its `TaskContext` as the *first* constructor argument, and a member coroutine's implicit
object parameter already occupies that position, so a member coroutine body is not expressible with
the in-tree `Task` at all - the same limitation [`services/common-contract.md`](services/common-contract.md) section 1.1 records for
`Service::start`, verified again here with MSVC 14.44. **The spec won on shape**: `NetClient::send`
returns that `Task<NetResult>`, and the coroutine body is a context-first static function.

The related, sharper disagreement is about *cancellation being a value*. If the returned task
carried the caller's `CancellationToken`, the runtime would check it before each resume, finish the
task with `TaskCancelled`, and the `expected` would never be produced - so `cancelled` would arrive
as an **exception**, contradicting both section 2's error-kind list and section 3 item 4's "yields
`cancelled` exactly once". **The spec's outcome vocabulary won**: the task's own token is empty, and
cancellation flows through the token the caller passes to `send`. The cost is that a `send` task is
not itself cancellable as a *task* (destroying it does not cancel the request); the caller cancels
the token or the ticket, which is what section 2's signature actually says.

**2. Section 2's table mentions `Request.stream` but never declares it.** The `Response` row says
"`body` (bytes, or a stream when `Request.stream` is set)", and the `Request` row has no `stream`
field. There is therefore nothing to implement, and this slice does not invent one: `NetResponse::
body` is always a buffer. Recorded here rather than guessed, because a streamed body changes the
lifetime of the result (a callback per chunk, not one completion) and that is a spec change, not an
implementation choice.

**3. Section 3 item 7's header rule needed a decision the spec does not state.** "The library adds
no `User-Agent` beyond a fixed `Tessera/<version>`" does not say what happens when the caller sets
its own. The implementation adds the default **only when the caller has not**, which is the reading
that keeps a caller's explicit header authoritative - the alternative (always overwrite) would make
the field unsettable. `NetClientConfig::send_default_user_agent = false` removes it entirely.

Two further places where the *code* had to pick a side of an ambiguity, both resolved towards the
stricter behaviour:

- **`offline` vs `connect`** (section 3 item 6) is decided by the back end, because only the platform
  can tell them apart. `FakeNet::set_offline` reports `offline`; a scripted `push_error(Connect)`
  reports `connect`. The suite asserts the two are different kinds, which is the rule's whole point.
- **A `3xx` the spec does not name** (`300`, `304`, `309`) and a `3xx` with no `Location` are *not*
  followed: they are delivered as a `NetResponse` and the caller decides, exactly as
  `RedirectPolicy::Manual` does. Section 3 item 2 names five statuses, so following more would be
  inventing policy.

## 3. The state machine

`NetStateMachine` is a value type with no I/O, no time and no allocation, so the transitions are
testable on their own (`appfw_net_state_tests.cpp`). `NetClient` decides *when* to call them.

```text
Idle --dispatch--> Loading --response--> Done      (Ok for 2xx, HttpStatus otherwise)
                      |-----error-----> Error
                      +--cancel-------> Error      (outcome Cancelled)
                      +--timeout------> Error      (outcome Timeout)
                      +--redirect-----> Loading    (a new hop; `attempts` increments)
```

- **`Idle` is a real state.** `submit` returns with the request in `Idle` and the attempt queued on
  the `IoTaskExecutor`; that is what "never blocks the UI thread" means operationally, and the suite
  asserts the back end has seen nothing yet.
- **`Done` covers 4xx and 5xx.** Section 2: "A `Response` with a 4xx or 5xx `status` is **not** an
  error at this layer". `NetOutcome::HttpStatus` keeps that distinguishable from `Ok` without
  putting it in `Error`.
- **`Error` is five outcomes, not one.** `Timeout`, `Cancelled`, `Transport` (offline/dns/connect/
  tls), `Limit` (too_large/too_many_redirects) and `Request` (bad_url/unsupported/other). The first
  three are the ones the acceptance names: a caller retries the first, drops the second, and shows
  the third.
- **Terminal is final.** `NetStateMachine::finish` is the only terminal transition, so a late
  transport completion cannot overwrite a cancellation. It is dropped and counted
  (`NetDiagnostics::stale_completions`) rather than silently forgotten.

**How a request is cancelled.** Three entries, one outcome, exactly once:

1. **The caller's token** (`send`/`submit`). It is delegated to the attempt's own
   `CancellationSource`, so the back end observes it and releases its connection (section 3 item 4).
   `NetClient::tick()` then resolves the request at the frame boundary even if the back end ignores
   it - so a cancelled request cannot outlive one tick, which is the "within one loop tick" half of
   the rule that *is* enforceable here.
2. **`cancel(ticket)`**, the explicit form, for a screen that unmounts. It resolves the request,
   cancels the attempt token and cancels the deadline timer in one call. A second call returns false.
3. **`tick()`**, the backstop: it resolves every request whose token was cancelled, and every request
   whose deadline passed while its timer could not be armed because the timer queue was full.

Every path goes through `resolve`, which cancels the attempt token, disarms the deadline, applies the
single terminal transition and delivers once. "A cancelled request never delivers a body" therefore
holds structurally: the body is written into the record only by `resolve` with a response, and a
resolved record refuses every later `accept`.

## 4. Time: a timer, not a sleep

The per-attempt deadline is one `TimerQueue::schedule_one_shot` at
`clock.now_seconds() + timeout_ms/1000`, so:

- it costs nothing per frame (an idle app parks on `RuntimeLoop::next_timer_wait_seconds()`);
- it is driven by `ManualClock` in tests, so "the request timed out" is advanced to rather than
  waited for;
- it restarts per hop, because section 3 item 3 says "per attempt" - a slow chain of fast redirects
  does not time out, and the suite asserts exactly that;
- if the queue is full, the deadline still fires from `tick()`'s clock comparison, so a bounded
  timer queue degrades the *wake*, never the rule.

**There is no retry.** Section 3 item 3 ends with "There is no implicit retry", so this module
deliberately has no backoff, no schedule and no retry counter. The backoff policy the design doc
(services/platform-modules.md section 5) mentions belongs to `async-data-v1.md` section 3, which owns "timeouts/
backoff/retry for idempotent reads"; `http_method_is_idempotent` is the fact that policy branches on,
exported from `net_types.hpp` so the two cannot disagree. Wiring it would mean a timer-per-backoff
in the query layer above, not in `net` - adding one here would contradict the normative sentence.

## 5. "Never block the UI thread", structurally

| Claim | What makes it true |
|---|---|
| `send`/`submit` do no I/O | the attempt is posted as an `IoTask`; the work body is the only code that calls `NetTransport::start` |
| nothing completes inline | the transport's completion is posted to the `CompletionQueue` by the module, even if a back end calls it on its own thread |
| the caller's callback runs on the UI thread | the record is resolved only from `CompletionQueue::run` or from `tick()` |
| a pending request is not a render cause | `RuntimeLoop` presents only on damage; the suite drives the real loop for 30 frames and asserts zero presents |
| a real back end cannot block the caller | it receives an `IoTaskExecutor` and a token, and contract 1 in `net_transport.hpp` forbids an inline completion |

The executor is AF7's `runtime::IoTaskExecutor` seam, unchanged. That is deliberate: a second
"background" mechanism for networking would be a second place where the UI thread could be blocked,
and the storage module already proved the seam's shape.

## 6. Test map

One executable, `appfw_net`, five groups, one TU each so a failure names the group.

| Group | Proves |
|---|---|
| `appfw_net_state_tests` | every transition; a 4xx/5xx is `Done`; terminal is final; the kind→outcome mapping is total |
| `appfw_net_error_tests` | timeout, cancel (explicit and token), a late completion dropped, offline ≠ connect, dns/connect/tls, too_large, the redirect loop, an unavailable back end, destruction mid-flight |
| `appfw_net_redirect_tests` | 301/302/303 rewrite and 307/308 preserve, cross-origin drops `Authorization`, same-origin keeps it, https→http refused, `manual`, an unlisted 3xx, a relative target, the `User-Agent` rule, cleartext refusal, redaction, `bad_url` |
| `appfw_net_thread_tests` | `submit` does no work, the three stages stay separate, a frame tick completes while a request is pending, zero presents for a pending request, the deadline is a timer the loop wakes on, the in-flight bound refuses |
| `appfw_net_request_tests` | the section 2 defaults as values, header case-insensitivity, `send` through a coroutine (value, immediate result, cancellation, refusal), the record bound, `release`, the bounded redacted diagnostics ring |

Determinism: `ManualClock`, `ManualIoExecutor`, `FakeNet`, a hand-fired `TimerQueue` and a
hand-drained `CompletionQueue` throughout. No test opens a socket, reads a wall clock, sleeps, starts
a thread or touches the file system.

**Sensitivity.** The suite is written so a plausible implementation mistake fails it rather than
passing: dropping the `Authorization` rule fails two assertions; collapsing `timeout` into `other`
fails three; delivering a 5xx as an error fails four; resolving inline instead of posting fails
`submit_returns_before_any_network_work` and the three-stage test; a retry-by-sleep would have no
timer to assert against `next_timer_wait_seconds() > 29.0`.

## 7. Gaps the seam leaves, and how it accommodates them

| Gap | Why it is a gap | How the seam absorbs it |
|---|---|---|
| the five real back ends | one PR each; dependency entries first (ADR-052 item 5) | one `NetTransport` subclass per platform; nothing above the seam changes |
| TLS / HTTP/2 / HTTP/3 | negotiated by the platform stack, which is the point of ADR-052 item 1 | `NetProtocol` reports what the stack chose; a TLS failure is the `tls` kind; no API here can disable validation (section 3 item 8) |
| WebSocket / SSE | section 1 non-goals: a separate interface | a second interface over the same completion queue, not a change to `NetTransport` |
| streamed bodies | section 2 never declares `Request.stream` (see section 2 above) | a streamed form needs a chunk callback in `NetTransport::Completion`; the seam is one class, so that is an additive change |
| relative redirect targets | RFC 3986 reference resolution is not implemented; refused as `unsupported` | every real back end resolves references internally, so this affects the fake path only; a `resolve_reference(base, location)` function would sit beside `parse_url` |
| proxy / PAC | the OS configuration belongs to the back end (ADR-052 item 5) | no field, because nothing above the seam may read system state (section 3 item 10) |
| certificate pinning | section 1 non-goal for V1 | an interface-level option each back end implements (app-framework-open-questions.md section 5, "seams to leave now") |

## 8. Open questions this slice raises

- **Who follows redirects across a real back end?** This module does, because section 3 item 2 is
  normative and the platforms disagree. That requires each back end to be configured to follow
  nothing (`WINHTTP_OPTION_REDIRECT_POLICY_NEVER`, `CURLOPT_FOLLOWLOCATION 0`, `redirect:
  "manual"`), which is a per-back-end task and a line in its slice. If a platform cannot be
  configured that way, the alternative is per-back-end redirect handling plus a contract-suite
  variant, which is strictly worse.
- **`Request.stream`** (section 2's table) needs a spec line before it needs code.
- **Cleartext policy location.** `net.allowCleartext` is a project setting in section 3 item 8; this
  slice takes it as `NetClientConfig::allow_cleartext` so the runtime half is testable before the
  project-config key exists. Wiring the key to the field is a one-line composition-root change, and
  `project-config-v1.md` remains the owner of the setting's name and default.
