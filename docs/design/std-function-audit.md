# `std::function` in hot paths: the audit (issue #707)

- **Status:** Complete
- **Date:** 2026-11-22
- **Issue:** #707 (found by the memory-model audit, #708)
- **Spec rule:** `docs/specs/memory-model-v1.md` section 2 — "avoid `std::function`/type erasure in
  hot paths; prefer function pointers/small callables".

## What the issue asked for

An audit, not a rewrite: classify every use as per-frame/per-event hot or cold, count calls per frame
for the hot ones, and replace only those that allocate or run per node per frame. Record the
classification in the PR.

## The count

`git grep -c 'std::function' -- src include` finds **108** uses (the issue's 95 was measured
2026-11-21; the tree has grown). Classified by the role of the file they live in:

| Class | Uses | Where |
|---|---|---|
| **Hot path** | 67 | `src/runtime/`, `src/app/`, `src/ui/accessibility/`, `src/ui/forms/`, `src/ui/overlay/`, `src/rendering/rhi/` |
| **Cold** | 32 | `src/compiler/`, `src/harness/`, `src/schema/`, `src/diagnostics/`, `src/appfw/`, `src/hal/`, `src/platform/`, `tools/` |
| **Unknown** | 9 | Vulkan internals (`vulkan_device_internal.hpp`) |

## The finding that decides the issue

**The per-frame paths contain zero `std::function`.**

```
git grep -c 'std::function' -- src/ui/frame src/ui/paint src/ui/layout src/ui/text
# (no output: 0 matches in all four)
git grep -c 'std::function' -- src/ui/style src/rendering/gpu
# (no output: 0 matches in both)
```

The frame loop, the paint pass, the layout engine, the text stack, the style cascade and the batched
2D renderer — the six things that run every frame — do not use type erasure at all. The one use in
`src/ui/scene/` is `OverlayHost::close_if`, a one-shot predicate called when an overlay closes.

So the 67 "hot" uses are hot *areas*, not hot *calls*. Every one of them is a **registration or
submission API**: the `std::function` is constructed once by the caller and stored, then invoked
later without allocating.

| Use | Constructed | Invoked | Allocates per frame? |
|---|---|---|---|
| `TimerQueue::schedule_one_shot/periodic` | at schedule | `callback()` in `fire_due` | **No** — the call is a plain invocation |
| `AsyncRuntime::post` / `on_cancel` | at post | on the executor | **No** |
| `IoExecutor::submit` / `WorkerExecutor::post` | at submit | on the worker | **No** |
| `AnimationCallback` (`on_complete`) | at animation start | once, at completion | **No** |
| `AnimationEventListener` | at `add_event_listener` | per event, not per frame | **No** |
| `UiaHost` / `UiaBridge` callbacks | at bridge setup | per accessibility request | **No** |
| `FormModel` validators | at form setup | per validation | **No** |

Two checks confirm this rather than assuming it:

- **No hot path constructs a `std::function` per call.** `git grep 'std::function<' -- src/ui src/app
  src/rendering` filtered for `= [`, `= std::bind`, `push_back(`, `emplace_back(` returns nothing.
- **No hot path copies one per call.** The by-value parameters (`post`, `submit`, `schedule_*`,
  `on_cancel`) are *submission* APIs where by-value is the ownership transfer into the callee's
  storage — the copy is the point, not overhead. Every *invocation* path takes the stored object by
  reference.

## What was already right

The codebase had already adopted the pattern the spec asks for, in the places where it matters:

- `InputPump::GestureCallback` is `void (*)(void* user, const GestureEvent&)` with an explicit
  comment: "a plain function pointer plus opaque user data keeps the per-event path allocation-free
  (no `std::function` target to store)".
- `SettleHooks` and `ContainerSizeFn` use the same shape.
- `FramePipeline`'s `NodeLookupBuffer` is a retained vector rather than a per-frame map.

So the "prefer function pointers/small callables" rule is not aspirational here; it is the
established convention on the paths that run per frame, and `std::function` is confined to
registration seams where a stored callback is the right tool.

## What this audit changes

**Nothing in the code.** That is the honest outcome: the issue's premise was that some of the 95 uses
would be hot and allocating, and the measurement says none are. Replacing a stored `std::function` in
a submission API with a function pointer plus `void*` would make the API worse (it would lose
capturing lambdas) for no measured gain.

## What it does add

A rule that keeps the finding true, in `docs/guides/code-style.md`: a `std::function` may be *stored*
(a registration or submission seam) but must not be *constructed or copied on a per-frame or
per-node path*. That is the distinction the spec's one-line rule leaves implicit, and it is the one
that decides whether a given use is a problem.

## Follow-ups this audit does not cover

- The 9 `unknown` uses in `vulkan_device_internal.hpp` are device-creation and recovery callbacks,
  which run on device loss rather than per frame. They are cold by inspection but were not traced
  through the backend, which is why they are recorded as unknown rather than cold.
- `AnimationRuntime::tick` returning its result by value is a real per-frame allocation, but it is a
  *return value*, not a `std::function`. It is tracked on #701 and pinned by the allocation gate
  (#800).

## References

- `docs/specs/memory-model-v1.md` section 2.
- #708 (the audit this came from), #701 (the allocation measurement and gate), #800 (the gate).
- `include/tessera/ui/input_pump.hpp` — the function-pointer precedent.
