# Memory model v1

Status: **accepted** (2026-11-22). Normative companion to
[`threading-and-memory.md`](../design/threading-and-memory.md) and
[`resources.md`](../design/resources.md). Bridge policy:
[ADR-024](../../decisions/ADR-024-native-first-boundaries.md) (native-first boundaries; no
production bridges) and [`bridges.md`](../design/bridges.md).

Promoted from *proposed* once the allocator question was decided
([ADR-070](../../decisions/ADR-070-frame-arena-not-pmr.md)) and the audit's checklist was worked
through ([#708](https://github.com/thnak/Tessera/issues/708)). The rules below are what the code is
held to; where a rule is not yet enforced by a check, section 11 says so rather than implying it is.

Goals: deterministic allocation behavior, bounded memory, no leaks, no cross-language
ownership, no hidden allocations in the frame hot path, and explicit GPU memory budgets.
Buffer defaults/tunables are in [`buffer-config-v1.md`](buffer-config-v1.md).

## 1. Ownership classes

| Class | Representation | Rules |
|---|---|---|
| Value | small trivially copyable structs | pass by value; no lifetime |
| Unique | `std::unique_ptr<T>` | single owner; default for objects |
| Shared | `std::shared_ptr<T>` | only genuinely shared lifetime; never in hot path |
| Borrowed | `std::span`, `std::string_view`, `T*` non-owning | caller guarantees lifetime |
| Handle | stable id/index (`NodeId`, `TextureId`) | no pointer across threads/bridges |
| GPU handle | device-owned resource id | device outlives resources; retire after frames |

Rules:

- No raw owning pointers.
- UI nodes own children; components own state; device owns GPU resources.
- `useRef`/node handles are non-owning and cleared on unmount.
- Async callbacks capture handles/ids, never raw UI pointers.
- Cross-thread/cross-bridge data uses values, handles, or explicit shared memory—never borrowed
  language objects.

## 2. Allocator classes

| Allocator | Lifetime | Use |
|---|---|---|
| System | process | startup, rare large allocations |
| Frame arena | one frame | temporary per-frame data, command scratch |
| Persistent arena | app | long-lived tables, interned ids, style rules |
| Pool | app/frame | fixed-size nodes/commands/descriptors |
| GPU allocator (VMA, Vulkan Memory Allocator) | device | buffers/images/staging |
| Bridge scratch | one callback | JNI (Java Native Interface) / ObjC (Objective-C) / Wasm (WebAssembly) marshalling, reset after callback |

Rules:

- No `malloc`/`new` in steady-state frame code after warm-up.
- Frame arena resets at frame boundary; persistent arena is not reset per frame.
- Pools have fixed capacity and fail with a diagnostic, not silent growth.
- `std::pmr` may back arenas behind project aliases; do not expose `std::pmr` in public APIs.
- Avoid `std::function`/type erasure in hot paths; prefer function pointers/small callables.
- Intern strings/ids at build/config time; no formatting in the frame loop.

## 3. Frame lifetime

```text
begin frame -> frame arena reset
  -> style/layout/paint/command scratch
  -> submit -> completion
  -> reset arena; retire deferred resources
```

- Data referenced by submitted commands must outlive the GPU submission; use persistent/GPU
  allocations or copy into a frame-owned buffer that is not reset until completion.
- Deferred deletion: resources in flight are not destroyed; retire after fence/timeline value (GPU completion signals).
- Async results are moved into UI-owned state at a frame boundary; no dangling references.
- Resize/swapchain keeps old targets until in-flight frames retire
  ([ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md), window resize and
  lifecycle).

## 4. GPU memory

- VMA sub-allocates; no raw `vkAllocateMemory` in the backend.
- Categories: vertex, index, uniform, texture, staging, render target, glyph atlas, path cache.
- Uploads use pooled staging buffers; batch per frame; no per-draw allocation.
- Budgets are explicit per category; eviction is LRU (least recently used)/age-based and never evicts in-flight data.
- OOM (out of memory) degrades: evict caches, reduce optional quality, skip prefetch; fatal only if the device
  cannot continue.
- Track bytes, peak, fragmentation, staging bytes, and eviction counts.

## 5. Caches

Bounded caches with keys and eviction:

| Cache | Key | Eviction |
|---|---|---|
| Glyph atlas | face/size/glyph | LRU + frame-age guard |
| Shaped run | font/size/features/script/text | LRU |
| Layout measure | text/style/constraints | LRU |
| Image/texture | asset id/decode params | LRU |
| Pipeline | shader/layout/blend | refcount + LRU |
| Path | path hash/quality | LRU |

- No unbounded cache growth under animation/scroll.
- Cache budgets are configurable (see [`buffer-config-v1.md`](buffer-config-v1.md)); defaults
  differ desktop vs embedded (the values are an open question, section 11).
- Eviction must not change pixels for the current frame.

## 6. Bridge memory

A bridge is any boundary that marshals data/control to another runtime (JNI, Objective-C/Swift,
JS/Wasm, IPC, plugin ABI). The native path must not allocate/copy per frame through a bridge.

Rules:

- No UI objects cross a bridge; use value structs/handles.
- Define ownership at the boundary: copy-in, copy-out, or explicit shared memory.
- Bridge scratch memory is per-callback and reset; no long-lived language objects.
- No exceptions across FFI; errors are values.
- No per-frame serialization/JSON; batch, use binary/shared memory, or call native directly.
- Bridge calls are measured; a per-frame bridge call is a design smell.

### Android/JNI

- `JNIEnv*` is thread-local; attach/detach worker threads explicitly.
- Cache class/method ids; use `RegisterNatives`; minimize `FindClass`/`GetMethodID`.
- Local refs are bounded; delete in loops; use `PushLocalFrame`/`PopLocalFrame` for batches.
- Use global refs only for long-lived Java objects; release on shutdown.
- Prefer direct `ByteBuffer`/native pointers (`ANativeWindow`, `AInputQueue`) over per-event JNI.
- Keep `GetPrimitiveArrayCritical` sections short; do not call back into Java inside them.
- Run CheckJNI in debug; test local-ref overflow and detach paths.

### iOS/Objective-C/Swift

- Use a thin Objective-C++ adapter; C++ owns the engine, ObjC owns the view/controller.
- Per-callback `@autoreleasepool`; no unbounded autoreleased objects in the frame loop.
- Use `CFData`/`NSData` no-copy only when the buffer's owner and lifetime are defined at the
  boundary (section 6 rule "Define ownership at the boundary"); define who owns the buffer.
- ARC handles ObjC lifetime; C++ objects must not hold unretained ObjC pointers. The rule is to avoid
  it; a C++ member that needs an ObjC object holds a strong reference and releases it in its
  destructor, and a callback that outlives the call passes an id rather than a pointer (section 1,
  "handles not pointers across threads").
- Avoid per-frame ObjC message sends in the render path; cache callbacks/pointers.
- `CAMetalLayer`/drawable ownership follows the view/scene lifecycle.

### WebAssembly/JS (design-time only)

- Not part of production; no JS bridge in native apps.
- Prefer typed arrays/SharedArrayBuffer/WebGPU; avoid JSON per frame.
- The IPC bridge is versioned, bounded, and validated; no arbitrary filesystem/network access.
- Wasm memory is separate; copy at explicit boundaries; no shared pointers.

See [`bridges.md`](../design/bridges.md) for the native-first bridge policy.

## 7. OOM and failure

- Recoverable allocation failure returns an error value (`std::expected<T, Error>`, see
  [error-model-v1.md](error-model-v1.md) and [AGENTS.md](../../AGENTS.md#code-conventions) "Code conventions").
  Unrecoverable out-of-memory (`bad_alloc`) is fatal and goes to the root handler.
- Evict caches, drop optional quality, skip prefetch, retry once; then surface a diagnostic.
- Never crash on a cache allocation failure if a fallback exists.
- Fatal OOM triggers controlled shutdown, not undefined behavior.
- Log bytes/category and the failed allocation size; redact user data.

## 8. Determinism

- Allocation behavior must not affect output (no address-dependent ordering).
- Stable container iteration; no hash-map iteration in output paths.
- Frame arena high-water mark is measured, not assumed.
- Golden mode uses fixed sizes/fonts/DPI and no random ids.

## 9. Metrics

- Allocations/frame and bytes by category.
- Frame arena high-water mark; pool usage/failures.
- GPU memory by category, peak, staging bytes, evictions.
- Cache hit/miss/eviction/size.
- Bridge calls/bytes, JNI local refs, ObjC autorelease pool size, IPC queue depth.
- Leak/retirement backlog at shutdown.

## 10. Tests

- Allocation counters: steady-state frame allocates zero after warm-up.
- ASan/LSan/TSan (AddressSanitizer, LeakSanitizer, ThreadSanitizer); leak tests for nodes/resources/bridges.
- OOM injection: caches/queues degrade, no crash.
- In-flight resource retirement: old target/upload survives until completion.
- JNI local-ref overflow, global-ref release, thread attach/detach.
- ObjC autorelease pool growth; C++/ObjC lifetime tests.
- Wasm/IPC buffer bounds/ownership tests.
- Long-run scroll/animation: bounded memory, no cache growth.

## 11. Enforcement status

Which rules a check holds today, and which are held by review alone. A rule with no check is not a
weaker rule; it is one a reviewer has to catch, and saying so is better than implying a gate exists.

| Rule | Held by |
|---|---|
| No `malloc`/`new` in steady-state frame code | `alloc_per_frame_tests` + `scripts/check_alloc_budget.py` (per-lane ratchet, #800) |
| Pools fail with a diagnostic, not silent growth | `scripts/check_memory_budget.py` (counted rows at 0%) |
| Avoid `std::function` in hot paths | review; the rule is stated in [`code-style.md`](../guides/code-style.md) and the audit is [`std-function-audit.md`](../design/std-function-audit.md) (#707) |
| Intern strings/ids; no formatting in the frame loop | review |
| Frame lifetime: retire after fence, deferred deletion | `tests/gpu/` retirement and in-flight tests |
| VMA, no raw `vkAllocateMemory` | review; one known violation is #704 |
| Eviction never touches in-flight data | `ImageTextureCache` and glyph-atlas tests |
| Bounded caches | `scripts/check_memory_budget.py` cache rows |
| Recoverable failure as `std::expected` | `src/ui/boundary/guard.cpp` and its tests |
| Steady-state RSS and `sizeof` ratchet | `scripts/check_memory_budget.py` (two-sided) |
| ASan/LSan/TSan | CI lanes (#702) |
| Determinism (section 8) | goldens on lavapipe; hash-map iteration in output paths is **not** checked |

## 12. Open questions

- ~~Custom arena API vs `std::pmr` wrappers.~~ **Decided:** a custom arena API, not `std::pmr`
  ([ADR-070](../../decisions/ADR-070-frame-arena-not-pmr.md)). The measurement that decided it is in
  that ADR: an idle frame already allocates zero, and every remaining site outlives the frame, so the
  arena's first customer is command scratch rather than the cascade.
- Default budgets per platform (desktop/embedded).
- Zero-copy image/texture upload paths per backend.
- Shared-memory bridge for design-time preview.
- GPU memory fragmentation/defragmentation policy.
- Whether to expose allocator metrics in release builds.
