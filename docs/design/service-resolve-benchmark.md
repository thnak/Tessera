# Service-resolve benchmark (ADR-050)

Status: measurement record (2026-11-12). Backs the service-resolution decision in
[ADR-050](../../decisions/ADR-050-opt-in-app-framework-layer.md) section 3. The benchmark is a
standalone tool in `tools/poc/service_resolve/` ([`main.cpp`](../../tools/poc/service_resolve/main.cpp)); it links
nothing from Tessera, is off by default (`TESSERA_BUILD_SERVICE_RESOLVE_BENCH`) and is not
registered with ctest.

## 1. Question

ADR-050 lets components and services reach app-scoped services through a `ServiceContext` keyed by
compile-time ids. The owner asked whether the resolve step costs performance and where it may run.
The benchmark compares:

| Case | What it models |
|---|---|
| (a) lookup per call | frozen fixed-capacity registry (sorted `{id, type tag, pointer}`, binary search, id and tag compared) queried on every use |
| (b) resolved at mount | the same registry, queried once at mount; the typed pointer is cached in the component |
| (c) constructor injection | generated code passes typed pointers into the component; no lookup at all |
| (d1) `unordered_map<type_index>` | naive locator keyed by `std::type_index` (needs RTTI) |
| (d2) `unordered_map<string>` | naive locator keyed by a `std::string` built per call |
| (e) dispatch | trivial service method called statically (inlined), through CRTP, statically but not inlined, and through a `virtual` interface |

## 2. How to run

```powershell
cmake -S tools/poc/service_resolve -B D:/b/svcresolve -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build D:/b/svcresolve
D:/b/svcresolve/service_resolve_bench.exe        # optional argument: iteration scale factor
```

From the full tree the same target builds with `-DTESSERA_BUILD_SERVICE_RESOLVE_BENCH=ON`. On
MSVC run the commands from a `vcvars64` environment.

## 3. Method

- 64 distinct service types (`Svc<0>` .. `Svc<63>`), each `final` and implementing a one-virtual
  interface, so a call through the concrete type is static dispatch and a call through the
  interface is virtual. The method adds to a member and returns it.
- Each loop iteration starts with a compiler-only memory barrier (`_ReadWriteBarrier` on MSVC,
  an empty `asm volatile` with a memory clobber elsewhere). Without it the optimizer hoists a
  loop-invariant lookup out of the loop and every case measures the same thing. The barrier also
  forces the cached pointer to be reloaded from its owner each iteration, which is what real code
  does across calls.
- Per-call cases run 20 000 000 operations; per-mount cases mount 4 096 or 262 144 components
  into preallocated storage. Every case runs once to warm up, then 9 timed repetitions; the
  tables report the median (and run 2's median) in nanoseconds per operation.
- Allocations are counted by replacing the global `operator new`/`delete`; the count covers only
  the timed body. "allocs/op" is the worst repetition.
- Cache-effect cases visit K components (64 bytes each) in a fixed pseudo-random order, one call
  per component, so large K misses the cache on the component itself.

Machine: AMD Ryzen 5 4600H (Zen 2, 6 cores / 12 threads, 3.0 GHz base with boost enabled, 3 MB L2
total, 8 MB L3), 40 GB RAM, Windows 11 Home 10.0.26300, MSVC 19.51.36260 x64, CMake Release
(`/O2 /Ob2 /DNDEBUG`), Ninja. No core pinning, default power plan, laptop on AC.

## 4. Results

### 4.1 Per call (one resolve plus one trivial call), ns per call

| Case | 8 services: run 1 | run 2 | 64 services: run 1 | run 2 | allocs/op |
|---|---:|---:|---:|---:|---:|
| baseline: empty loop with barrier | 0.44 | 0.43 | 0.41 | 0.41 | 0 |
| (a) registry lookup every call | 5.32 | 5.23 | 8.04 | 8.01 | 0 |
| (b) resolved at mount, cached typed pointer | 2.17 | 2.13 | 2.16 | 2.18 | 0 |
| (c) constructor-injected typed reference | 2.17 | 2.11 | 2.10 | 2.08 | 0 |
| (d1) `unordered_map<type_index>` every call | 20.47 | 19.80 | 40.37 | 40.23 | 0 |
| (d2) `unordered_map<string>` every call | 66.72 | 65.59 | 65.45 | 66.31 | 1 |

### 4.2 Dispatch through the service interface, ns per call

| Case | run 1 | run 2 | allocs/op |
|---|---:|---:|---:|
| static call on `final` type (inlined) | 2.12 | 2.10 | 0 |
| CRTP static dispatch (inlined) | 2.11 | 2.09 | 0 |
| static call, not inlined | 1.78 | 1.81 | 0 |
| `virtual` call through the interface | 1.88 | 1.81 | 0 |

### 4.3 Per mount (registry of 64 services; one component needing N services), ns per mount

| N | M components | (b) registry lookup x N: run 1 / run 2 | (c) copy N injected pointers | (d1) `type_index` map x N | allocs/op |
|---:|---:|---:|---:|---:|---:|
| 1 | 4 096 | 6.91 / 7.10 | 0.73 / 0.76 | 19.12 / 19.31 | 0 |
| 8 | 4 096 | 61.82 / 58.72 | 3.00 / 6.32 | 153.52 / 160.08 | 0 |
| 64 | 4 096 | 640.38 / 657.54 | 10.11 / 10.11 | 1507.01 / 1665.80 | 0 |
| 1 | 262 144 | 6.79 / 7.26 | 0.80 / 0.93 | 19.35 / 19.97 | 0 |
| 8 | 262 144 | 63.10 / 63.05 | 6.62 / 7.69 | 158.97 / 157.27 | 0 |
| 64 | 262 144 | 648.65 / 648.43 | 61.57 / 65.56 | 1540.67 / 1567.50 | 0 |

One-time cost: `provide` of 64 services plus `freeze` (sort): 1 629 / 1 664 ns per registry, no
allocation.

### 4.4 Cache effects: one call per component over K components, shuffled, ns per call

| K | baseline: touch component only | (a) lookup every call | (b) cached pointer | (a) minus (b) |
|---:|---:|---:|---:|---:|
| 64 | 1.34 / 1.37 | 9.50 / 9.56 | 2.07 / 2.13 | 7.4 |
| 4 096 | 1.43 / 1.49 | 10.34 / 10.25 | 4.05 / 4.08 | 6.2 to 6.3 |
| 262 144 | 12.19 / 13.38 | 84.09 / 84.62 | 58.10 / 58.94 | 26.0 |
| 1 048 576 | 13.46 / 15.68 | 99.64 / 102.97 | 70.33 / 67.64 | 29.3 to 35.3 |

## 5. Reading the numbers

1. **Resolving once at mount makes the use site as cheap as injection.** (b) and (c) are the same
   machine code at the call (load a pointer from the component, call): 2.1 ns both, identical
   within noise. Neither allocates.
2. **A lookup per call is measurable, and worse when cold.** The frozen registry adds about
   3 ns (8 services) to 6 to 7 ns (64 services) per call when everything is in cache. When the
   component itself misses the cache (K >= 262 144) the lookup adds 26 to 35 ns, more than the
   in-cache difference, because the binary search waits behind the component load and reduces
   memory-level parallelism. That is the case that matters for a frame touching many components.
3. **Mount cost is small and linear.** The registry costs about 7 to 10 ns per declared service at
   mount (6 binary-search steps over 64 entries); 8 services cost about 60 ns per mounted
   component. A screen that mounts 1 000 components each declaring 8 services spends about 60 us,
   once, against a 16.7 ms frame. Injection is 10 to 20 times cheaper (under 1 ns per service in
   cache), but both are noise next to building nodes, style and layout at mount.
4. **Naive locators lose by a wide margin.** `std::type_index` hashing costs 20 to 40 ns per
   lookup with MSVC (its `type_info` hash walks the decorated name) and needs RTTI; a string key
   built per call costs about 66 ns and allocates once per call. Both are excluded.
5. **Virtual dispatch is not measurable here.** A monomorphic, well-predicted virtual call costs
   the same as a non-inlined static call (1.8 ns); both are as fast as the inlined call, which is
   bound by the store-to-load latency of the counter it updates. The real cost of `virtual` is lost
   inlining and cross-call optimization, which a one-line method cannot show, and it applies only
   where a method is called per frame.

## 6. Limits of this microbenchmark

- One machine, one compiler, one run configuration; no clang/GCC or ARM numbers. Absolute values
  will move on other hardware; the ratios are the useful part.
- The per-iteration barrier is pessimistic: real code between calls does other work and the
  optimizer may keep a pointer in a register. The cases are compared under the same barrier.
- The branch predictor sees the same service every iteration, which flatters (a), (d) and the
  virtual call. Mixed call sites with many services would cost more, not less.
- The trivial method is dominated by store-to-load forwarding on its counter (about 2 ns), so
  differences below roughly 0.3 ns are not meaningful (for example inlined versus not inlined).
- Mount storage is a plain array; real mounting also allocates nodes and runs style and layout,
  which this tool does not measure. Run 1 and run 2 differ by up to about 2x on the smallest
  injection case (N = 8, M = 4 096: 3.0 versus 6.3 ns), so single-digit numbers there are noisy.
- A dense compile-time slot index (array index instead of binary search) was not measured; it would
  make (a) and the mount cost of (b) cheaper, but the decision does not depend on it.
