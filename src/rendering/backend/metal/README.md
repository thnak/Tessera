# Metal backend

Native Metal backend for macOS and iOS. **No MoltenVK**: this is the Metal API directly, in
Objective-C++ confined to `src/rendering/backend/metal/`. Decision:
[ADR-053](../../../decisions/ADR-053-native-metal-backend.md). Design (synchronization, resource
lifetime, presentation): [`docs/design/gpu/metal-backend.md`](../../../docs/design/gpu/metal-backend.md).

## Status

**Tier 2 in-tree, in progress.** The API-free half (descriptor mapping tables) is implemented and
tested on every host. The device half is implemented for device, buffer, texture and render-target
creation; command recording, pipelines and the `CAMetalLayer` swapchain are the remaining slices
(ADR-053 slices 2 and 3).

Nothing here is a V1 deliverable: macOS and iOS are post-V1
([AGENTS.md](../../../AGENTS.md#v1-platform-scope-and-ci-policy)), so this is a contract to build
against.

## RHI version

Implements **RHI 1.7** (`rhi::kRhiVersion`), declared with a `static_assert` on
`rhi::kRhiVersionMajor` in `src/metal_device.mm`. RHI 1.7 added `Backend::Metal` itself.

## Platforms

- macOS 13 and later, iOS 16 and later (provisional floor; ADR-053 decision 1).
- Apple silicon and Intel Macs. Storage modes follow the design note's section 4.1: CPU-written
  buffers are `Shared`, GPU-only textures and render targets are `Private`, and `Managed` is never
  chosen.

## Building

The backend is off by default and in no preset. It needs an Apple platform and the Metal SDK:

```bash
cmake -S . -B build/metal \
  -DCMAKE_BUILD_TYPE=Release \
  -DTESSERA_BUILD_METAL=ON \
  -DTESSERA_BUILD_VULKAN=OFF \
  -DTESSERA_BUILD_POC=OFF
cmake --build build/metal --parallel 4
```

`TESSERA_BUILD_METAL=ON` on a non-Apple host is a configure error, not a silent skip: the device
half is Objective-C++ against `<Metal/Metal.h>`.

The mapping half (`tessera_metal_mapping`) builds on **every** host, so its unit test runs in the
ordinary Windows and Linux ctest:

```bash
ctest --test-dir build/dev -R "metal_mapping|retire_queue" --output-on-failure
```

## Layout

| Path | What it is |
|---|---|
| `include/tessera/rendering/metal/metal_mapping.hpp` | RHI -> Metal descriptor mapping. API-free: no `<Metal/Metal.h>`, no Objective-C. |
| `include/tessera/rendering/metal/metal_device.hpp` | The device factory. Plain C++: no `id<...>`, no `NS*`. |
| `src/metal_mapping.cpp` | The mapping tables, built everywhere. |
| `src/metal_device.mm` | Device, buffer, texture, render target. Objective-C++ with ARC. |
| `src/metal_internal.hpp` | Private to `src/`: the one place Metal types are allowed in a header. |

Objective-C objects are held with ARC inside `src/` and never appear in a header outside this
directory (design note section 4).

## How the mapping stays honest

`metal_mapping.hpp` pins Metal's documented numeric enumerator values so the mapping can be tested
without a Metal toolchain. `src/metal_device.mm` then `static_assert`s each pinned value against the
SDK's own enumerator. A wrong number is therefore a compile error on a Mac and a test failure
everywhere else, rather than a silently wrong pixel format at run time.

## Known gaps

- Command recording (`MTLCommandBuffer`, render/blit encoders), pipelines and the MSL shader path
  (ADR-053 slice 2).
- The `CAMetalLayer` swapchain, resize, present and DPI (slice 3).
- `supports_dual_source_blend()` reports **false** until the pipeline path proves the second
  fragment output per device. The documented fallback is grayscale coverage, so a caller is correct
  either way; reporting true without the proof would produce an undefined blend.
- `recover()` is not implemented: a lost device reports `NotSupported` and the caller recreates it.
- No `MTLHeap` and no untracked resources (ADR-053 decision 3).
- The conformance suite does not select Metal yet; it is gated on the macOS lane (slice 4).

## Tests

- `tests/unit/metal_mapping_tests.cpp` — every mapping, the pinned numeric values, the storage-mode
  policy and both blend states (which must match the Vulkan backend exactly). Runs on every host.
- `tests/unit/retire_queue_tests.cpp` — the shared retire rule the backend uses, with a fake
  completion source.
- Metal-only tests (a timeline test like `vulkan_timeline_frames`, a dropped-while-executing list, a
  `nil`-drawable skip, a drawable resize) need a Mac and arrive with slices 2 and 3.
