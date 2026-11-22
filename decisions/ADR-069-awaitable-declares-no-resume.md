# ADR-069: An awaitable declares when its frame will never resume

- Status: Accepted (2026-11-22).
- Date: 2026-11-22
- Related: [ADR-021](ADR-021-async-runtime.md) (async runtime), [ADR-050](ADR-050/README.md) (app
  framework layer), issue #770 (the leak this fixes), issue #753 (the sanitizer run that found it),
  [docs/design/async.md](../docs/design/async.md) (implemented coroutine subset),
  [docs/design/background-work.md](../docs/design/background-work.md) (cancellation while a child is
  suspended).
- Terms: a **frame** is a coroutine frame, the storage holding a suspended coroutine's locals. A
  **continuation** is the task that awaits another task and is resumed when it finishes. An
  **abandoned pair** is a driver task and the child it awaits when the owner has given up on both. A
  **parked** frame is one suspended on an awaitable that will never resume it.

## Context

`TaskStateBase::continuation_` is a `std::shared_ptr`. When a task awaits a child, the child stores a
strong reference to the awaiting task's state so it can resume it on completion. That is what makes
`co_await` work without the caller keeping the child alive, and it is why dropping the last `Task`
handle does not cancel a graph.

It also means an abandoned pair is a reference cycle. The driver frame's awaiter holds the child's
state; the child holds the driver's state as its continuation. If the child never finishes, nothing
breaks the cycle and both frames leak. `include/tessera/appfw/task_scope.hpp` documented that cost
rather than hiding it, and the `asan` lane found it: `appfw_task_scope` and `appfw_service` both
reported indirect leaks through `drive_child` and `drive_start` (#770).

The cycle cannot simply be broken by making `continuation_` a `weak_ptr`. The runtime cannot tell
"this child will finish later" from "this child will never finish", and the two need opposite
treatment. A child that will finish later must keep its awaiter alive, because it is going to resume
it. A child that will never finish must not, because the reference is the leak. Worse, destroying a
suspended frame is unsafe in general: the awaitable it is parked on may hold a bare
`coroutine_handle` and resume it later, which is exactly what the queue-hop fixture does. Destroying
the frame would then resume freed memory.

Only the awaitable knows which case it is. `ParkForever` discards the handle it is given and posts
nothing, so nothing can ever resume that frame. `QueueHop` posts `handle.resume()`, so something can.
The information exists at exactly one place in the program and the runtime cannot infer it.

## Decision

### 1. The awaitable opts in, from `await_suspend`

An awaitable that provably cannot resume the frame it is suspending calls
`runtime::detail::declare_no_resume(handle)` from `await_suspend`, passing the handle it was given.
The free function forwards to the promise, which forwards to the task state.

The contract is narrow and stated at the declaration: the awaitable holds no `coroutine_handle`,
posts nothing, and registers no callback. An awaitable that can resume later must not call it. This
is an opt-in, so the default stays the safe one: an awaitable that says nothing keeps the strong
reference and the frame stays alive.

### 2. The marker drops the continuation, and destroys nothing

`TaskStateBase::declare_no_resume()` releases `continuation_`. That is the whole effect. It does not
destroy the frame, does not cancel anything, and does not touch the queue.

Releasing the reference turns the cycle into a chain rooted at whoever started the awaiting task.
That owner already frees the awaiting frame at its next safe point - `TaskScopeState::reserve` for a
scope slot, `RunnerState::bridge` for a service attempt - and the child's state is freed with it,
because the awaiting frame's awaiter is the only remaining owner.

Nothing is destroyed at the marker because the frames must outlive the call. The awaiting frame is
still owned by its starter, and its awaiter still owns the child's state, so both survive the drop.
`self` pins the state across it in case releasing the continuation ever does cascade back through
the awaiter; the function runs inside the frame that state owns, so that would be a
use-after-free of the running frame.

### 3. The marker is not propagated

A task awaiting a task that never finishes will never resume either, so propagating the marker up
the chain looks tempting. It is not done, because the runtime cannot know that the middle frame is
itself parked: it may be suspended on a queue hop, in which case it will resume, observe its
cancelled token, and unwind normally. Propagating would drop a reference that is still needed.

The chain therefore unwinds one link at a time, each link released by its own owner. That is slower
to reclaim than a single sweep, and it is correct for every shape of graph rather than only the one
the tests happen to build.

## Consequences

- An abandoned pair parked on a never-resuming awaitable is freed at its owner's next safe point
  instead of leaking. `appfw_task_scope` and `appfw_service` pass under
  `ASAN_OPTIONS=detect_leaks=1` with no suppression.
- The default is unchanged. An awaitable that does not opt in keeps today's behaviour, including the
  cycle, so this cannot make an existing graph less safe.
- The marker is a promise the compiler cannot check. An awaitable that calls it and then resumes the
  frame anyway is a use-after-free, so the contract is documented at the declaration and the
  qualifying set is deliberately tiny: `ParkForever` in the two test fixtures.
- `task_scope.hpp`'s documented trade-off narrows. The cycle is now only formed by a child parked on
  an awaitable that *can* resume it, which is the case where retaining the frame is genuinely
  required.
- The `asan` lane goes green, which closes #771 and lets the nightly report on new findings instead
  of repeating this one.

## Alternatives

| Alternative | Why not |
| --- | --- |
| Make `continuation_` a `std::weak_ptr` | Frees the awaiting frame while the child is still suspended on it. A child that will finish later must keep its awaiter alive; it is going to resume it. |
| Destroy the abandoned frame at the deadline | Unsafe in general: the awaitable may hold a bare `coroutine_handle` and resume it later, which is a use-after-free. This is the reason the scope never destroys a suspended frame. |
| Propagate the marker up the chain | The runtime cannot tell a parked middle frame from one suspended on a resumable hop. Propagating would drop a reference that is still needed. |
| Suppress the leak in `lsan.supp` | That file is third-party only, and this is Tessera's own code. It would also hide the exact cost the header comment says should stay visible. |
| Bounded reclamation with the existing counters | Keeps the leak and only reports it. `stats.abandoned` and `tick()`'s `TimedOut` already do that; #770 asks for the cost to be paid down. |

## Follow-up

- `docs/design/async.md` section 2.1 states that dropping the last `Task` handle keeps both states
  alive until the child finishes. That is still true for a child that can resume; the never-resuming
  case now releases at the owner. The wording is updated with this ADR.
- A future awaitable that parks a frame forever should opt in. The two fixtures are the only ones
  today; a timer or a callback-based await must not.
