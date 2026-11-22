# Memory pressure and OOM degradation

Status: implemented for the image texture cache; the ordered entry point is the seam every other
cache registers with. Issue #705, [memory-model-v1](../specs/memory-model-v1.md) section 7.

## What the spec asks for

> Evict caches, drop optional quality, skip prefetch, retry once; then surface a diagnostic.
> Never crash on a cache allocation failure if a fallback exists.

Three separate obligations, and it is worth keeping them apart because they fail differently:

1. **An order.** "Evict caches" is not actionable until something says *which* cache first. Without
   an order, each cache evicts when it feels pressure, which is how a device ends up re-rasterizing
   glyphs while a 64 MiB image cache is still resident.
2. **A retry, bounded.** "Retry once" is a bound, not a suggestion. A retry loop on a full device is
   a hang.
3. **A diagnostic.** A degradation nobody can see is indistinguishable from a bug.

## The order, and why it is largest-first

The owner's decision on the issue was **largest-first strictly**:

| Rank | Cache | Default bound | Rebuild cost |
|---|---|---|---|
| 0 | `image_texture` | 64 MiB | re-decode from the source asset |
| 1 | `path_texture` | 32 MiB | re-rasterize from the path |
| 2 | `paint` | 2 MiB | recompute from the computed style |
| 3 | `glyph_atlas` | page limits | re-rasterize a glyph |
| 4 | `text_measure` | 1024 entries | recompute a measurement |

The two texture caches lead because they are both the largest and the cheapest to rebuild, so the
first step frees the most bytes for the least work. The glyph atlas is fourth rather than last
because it is larger than the measure cache; the measure cache is last because recomputing a
measurement is cheaper than re-rasterizing a glyph.

The order lives in `MemoryPressure::eviction_order()`, not in the registration sequence. A caller
that registers the measure cache first still sees the image cache evicted first, and
`memory_pressure_tests` asserts exactly that: the order is a property of the policy, not of the
caller. A cache whose name is not in the order is evicted **last rather than skipped** — a new cache
that nobody ordered is still better off evicted than resident.

## The interface is deliberately three callbacks

```cpp
struct EvictableCache {
    std::string_view name;
    std::function<std::uint64_t()> live_bytes;
    std::function<std::uint64_t()> evict_idle;
};
```

The pressure path does not know what a cache *is*. That is the point: it stays a pure policy with no
dependency on any cache, so every cache can link it without a cycle.

The cost of that generality is one rule the cache must honour itself: **`evict_idle` must not drop
anything the current frame's recorded commands reference.** memory-model-v1 section 5 says eviction
must not change pixels, and the pressure path cannot check that for the cache. A cache that cannot
tell what the current frame uses must not register, rather than registering and dropping pixels the
frame is about to draw.

That is why `ImageTextureCache` has two eviction entry points, not one:

- `evict_until_fits` runs inside `acquire`, while the frame is being recorded. It respects the
  parking lot, so a texture an already-recorded draw references stays alive until `end_frame()`.
- `evict_idle` runs between frames, from the pressure path. It drops every entry and reports the
  bytes.

## The bug this found in `ImageTextureCache`

The issue's own comment predicted it, and it was real: `evict_until_fits` ran **after**
`create_texture` succeeded. So the new texture was allocated while the cache still held its full
budget, and on a device near OOM the eviction that would have made room had not happened yet — the
creation failed for no reason.

The fix is two parts:

1. **Evict before creating.** The allocation now sees the memory it is about to be given.
2. **On failure, evict everything idle and retry exactly once.** A second failure returns
   `upload_failed`. `Stats::oom_retries` counts the recoveries, so a device that is persistently
   short is visible rather than silently slow.

## What the tests assert, and how they were checked

`image_texture_cache_tests` gained two cases:

- `oom_evicts_and_retries_once` — one injected OOM recovers and is counted; two injected OOMs
  surface as `upload_failed` and the cache still works afterwards.
- `evict_idle_releases_everything` — the pressure hook reports the bytes it released and leaves the
  cache empty.

The first one was **verified to bite**: deleting the retry block makes it fail on three assertions
("the recovered texture is cached", "a failed retry does not count as a recovery", "the cache still
works after a surfaced OOM"). A test that passes with the fix removed is not a test.

The injection seam is `FakeDevice::throw_textures`, which throws the way a real backend does
(`check_vk` on `vmaCreateImage`), alongside the pre-existing `fail_textures` that returns null. Both
shapes are real: a backend can report OOM either way, and the cache handles both.

On the Vulkan side, `InjectedDeviceFault::OutOfMemoryOnCreateTexture` fails the next
`vmaCreateImage` with `VK_ERROR_OUT_OF_DEVICE_MEMORY` while leaving the device `Ready` — an OOM is
recoverable, unlike the loss faults that share the enum.

## What is not done

- **The caches have no owner in `src` yet.** `ImageTextureCache` and `PathTextureCache` are
  libraries with tests; nothing in `src/app` or `src/ui/frame` constructs one. So the pressure path
  is the seam and the registration is the caller's job, not something this change could wire up
  without inventing an owner. When the renderer owns the caches, it registers them once at
  construction.
- **Only the image cache has the evict-and-retry.** The path cache and the glyph atlas need the same
  audit; the issue says so and this change does not claim otherwise.
- **`drop optional quality` and `skip prefetch`** are not implemented. They are separate mechanisms
  (quality tiers, prefetch scheduling) and neither has a consumer yet.
