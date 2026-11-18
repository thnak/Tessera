# Metal backend

Native Metal backend for macOS and iOS. **No MoltenVK**: this is the Metal API directly, in
Objective-C++ confined to `src/rendering/backend/metal/`. Decision:
[ADR-053](../../../decisions/ADR-053-native-metal-backend.md). Design (synchronization, resource
lifetime, presentation): [`docs/design/gpu/metal-backend.md`](../../../docs/design/gpu/metal-backend.md).

## Status

**Tier 2 in-tree, in progress.** Implemented: the API-free half (descriptor mapping tables and the
MSL shader table), and the device half for device, buffer, texture, render-target, command-list,
pipeline and `CAMetalLayer` swapchain. Not yet validated on hardware: no Mac is available on the
development lanes, so the Objective-C++ is syntax-checked against a stub header and the API-free
half is unit-tested on Windows and Linux.

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

The backend is off by default and in no preset. It needs an Apple platform, the Metal SDK, and
`TESSERA_SHADER_CROSS=ON` (the MSL shader table is built from the cross-compile path):

```bash
cmake -S . -B build/metal \
  -DCMAKE_BUILD_TYPE=Release \
  -DTESSERA_BUILD_METAL=ON \
  -DTESSERA_SHADER_CROSS=ON \
  -DTESSERA_BUILD_VULKAN=OFF \
  -DTESSERA_BUILD_POC=OFF
cmake --build build/metal --parallel 4
```

`TESSERA_BUILD_METAL=ON` on a non-Apple host is a configure error, not a silent skip: the device
half is Objective-C++ against `<Metal/Metal.h>`.

Without `TESSERA_SHADER_CROSS=ON` the MSL table is empty and `create_pipeline` fails with a clear
error rather than drawing nothing. That is deliberate: a Metal build with no shaders is not a
working build, and a silent empty pipeline would be much harder to diagnose.

The mapping half (`tessera_metal_mapping`) builds on **every** host, so its unit test runs in the
ordinary Windows and Linux ctest:

```bash
ctest --test-dir build/dev -R "metal_mapping|retire_queue" --output-on-failure
```

## Layout

| Path | What it is |
|---|---|
| `include/tessera/rendering/metal/metal_mapping.hpp` | RHI -> Metal descriptor mapping. API-free: no `<Metal/Metal.h>`, no Objective-C. |
| `include/tessera/rendering/metal/metal_shader_registry.hpp` | The MSL shader table. API-free. |
| `include/tessera/rendering/metal/metal_device.hpp` | The device factory. Plain C++: no `id<...>`, no `NS*`. |
| `src/metal_mapping.cpp` | The mapping tables, built everywhere. |
| `src/metal_shader_registry.cpp` | The MSL table, built everywhere. |
| `src/metal_device.mm` | Device, buffer, texture, render target. Objective-C++ with ARC. |
| `src/metal_command_list.mm` | Command recording: render/blit encoders, submission, completion. |
| `src/metal_pipeline.mm` | Pipelines from MSL, blend state, vertex layout. |
| `src/metal_swapchain.mm` | `CAMetalLayer`, acquire, present, resize. |
| `src/metal_internal.hpp` | Private to `src/`: the one place Metal types are allowed in a header. |
| `cmake/embed_msl.cmake` | Build-time MSL embedding (a configure-time read fails on a clean tree). |

Objective-C objects are held with ARC inside `src/` and never appear in a header outside this
directory (design note section 4).

## How the mapping stays honest

`metal_mapping.hpp` pins Metal's documented numeric enumerator values so the mapping can be tested
without a Metal toolchain. `src/metal_device.mm` then `static_assert`s each pinned value against the
SDK's own enumerator. A wrong number is therefore a compile error on a Mac and a test failure
everywhere else, rather than a silently wrong pixel format at run time.

## Known gaps

- **Nothing has run on a Mac.** The Objective-C++ is syntax-checked against a stub header on the
  Linux lane, which catches type and selector errors but proves nothing about linking or behaviour.
  The design note's **verify** items (bounded `waitUntilSignaledValue:` on the OS floor, an errored
  command buffer running its completed handler, drawable/frame-slot deadlock) are all still open.
- The MSL is compiled with `newLibraryWithSource:` at pipeline creation. That is once per pipeline,
  not per frame, and the source is a build-time artifact - but a precompiled metallib would remove
  the cost entirely and needs the Metal toolchain on the build host.
- `supports_dual_source_blend()` reports **false** until the pipeline path proves the second
  fragment output per device. The documented fallback is grayscale coverage, so a caller is correct
  either way; reporting true without the proof would produce an undefined blend.
- `recover()` is not implemented: a lost device reports `NotSupported` and the caller recreates it.
- No `MTLHeap` and no untracked resources (ADR-053 decision 3).
- Mip-chain generation is not implemented: `supports_texture_mipmaps()` reports **false**, so a
  caller samples the single level, which is the documented correct fallback. Flip it to true in the
  same change that records the downsample passes.
- The conformance suite selects Metal, but the entry skips on any lane without a Metal device, so
  it has not yet produced a passing run (slice 4 needs a macOS lane with a GPU).

## Tests

- `tests/unit/metal_mapping_tests.cpp` — every mapping, the pinned numeric values, the storage-mode
  policy and both blend states (which must match the Vulkan backend exactly). Runs on every host.
- `tests/unit/metal_shader_registry_tests.cpp` — the MSL table, in both the populated and the empty
  configuration. Runs on every host.
- `tests/unit/retire_queue_tests.cpp` — the shared retire rule the backend uses, with a fake
  completion source.
- `gpu_conformance --backend metal` — the backend conformance suite (ADR-049). Registered on every
  lane; it skips with a named reason where no Metal device exists, and a skip is not a pass.

## CI

The `metal` job in [`.github/workflows/macos.yml`](../../../../.github/workflows/macos.yml) is the
only lane that compiles the Objective-C++ device half. It installs `shaderc` and `spirv-cross` from
Homebrew (the MSL table needs them), configures with `TESSERA_BUILD_METAL=ON` and
`TESSERA_SHADER_CROSS=ON`, builds, and runs the mapping, MSL-table and retire tests plus the Metal
conformance entry.

It is advisory (nightly and on demand), like the rest of that workflow, and it does not claim
Tier 1: a hosted runner's GPU may not match hardware, so pixel goldens still come from lavapipe and
Metal is checked against `tolerance.json` only (ADR-009, ADR-053 decision 3).

The Windows and Linux lanes build the API-free half and unit-test it, and syntax-check the `.mm`
files against a stub Metal header. That catches type and selector errors but proves nothing about
linking or behaviour, so a green Windows/Linux run is not evidence that the backend works.
