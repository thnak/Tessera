# WebGPU backend (first slice)

Design note for `src/rendering/backend/webgpu`. Normative context: [ADR-002](../../../decisions/ADR-002-webgpu-dawn-for-wasm.md),
[ADR-011](../../../decisions/ADR-011-shader-authoring-and-cross-compilation.md), [backends.md section 4](backends.md). The
RHI contract is unchanged; this backend implements it.

## What the RHI requires of a backend

`include/tessera/rendering/rhi/{types,device,swapchain}.hpp` define the seam:

- `Device`: `create_buffer/texture/pipeline/render_target/command_list/swapchain`, `wait_idle`,
  `supports_sample_count` (1 or 4, never silently lowered), `status`/`recover` (device loss),
  `memory_statistics` (optional).
- `Buffer`: `write`/`read` with `std::out_of_range` on a bad range. `Texture`: whole-image `write`,
  `sample_count` (1 or 4; pixels only for 1x). `RenderTarget`: extent, format, sample count (4x
  resolves into a hidden 1-sample image that readback and sampling see).
- `Pipeline` is created from SPIR-V words (Vulkan) or a `shader_id` (other backends), a vertex
  layout, color format, `BlendMode`, a push-constant size, and a sample count that must match the
  target.
- `CommandList`: render pass begin/end (`RenderTargetDesc` carries clear color, load/store ops),
  bind pipeline/vertex buffer/texture slot, viewport, scissor, `push_constants`, `draw`,
  `copy_target_to_buffer`, `submit` (blocking, tests only), `submit_async` + `is_complete` +
  `wait(timeout)`. The caller keeps resources alive until completion.
- `Swapchain`: acquire/present/resize as values (`SwapchainError`), `image_target`.
- Rules: never block the UI thread, no hidden allocation or RTTI on the frame path, failures as
  values on hot paths, exceptions at boundaries.

## Mismatches and decisions

| Topic | WebGPU reality | Decision |
|---|---|---|
| Shaders | `PipelineDesc` holds SPIR-V; WebGPU takes WGSL; ADR-011 says WGSL is authored directly (no Tint/Naga at runtime) | `PipelineDesc::shader_id` (ADR-049) names the program; `ShaderRegistry` maps the id to an embedded WGSL module and each device starts with the built-ins (`batched2d`). An empty or unknown id fails pipeline creation with `std::invalid_argument`. The SPIR-V fields are ignored |
| Push constants | none | Uniform buffer at group 0, binding 0 with a dynamic offset; per command list arena, one 256-byte slice per distinct block (queue writes land before the whole command buffer, so one shared slot would show only the last value) |
| Binding model | bind groups | One group: 0 uniform, 1 texture, 2 sampler (`kBinding*` in the mapping header) |
| Clip space | y up (Vulkan is y down) | The WGSL vertex stage flips y |
| `LoadOp::DontCare` | no such op | Maps to `Clear` |
| Readback | `copyTextureToBuffer` rows must be 256-byte aligned; no sync map | `padded_bytes_per_row`/`readback_buffer_size`; `Buffer::read` throws, async map is a later slice |
| `submit()` | cannot block in a browser | throws `std::logic_error`; use `submit_async` |
| `wait_idle`/`wait` | no sync wait | no-op / poll only |
| Device creation | adapter and device are async | The Wasm host creates them and hands over handles (`create_device(void*, void*)`) |
| Swapchain | canvas surface is host-owned | `create_swapchain` returns `NotSupported` until a surface adapter lands |
| Unaligned `Buffer::write` | queue writes need 4-byte offset and size | rejected with `std::invalid_argument` |

## Layout

| Path | Role | Built |
|---|---|---|
| `include/.../webgpu/webgpu_mapping.hpp`, `src/webgpu_mapping.cpp` | Pure RHI -> WebGPU descriptor tables (formats, usage bits, blend, load/store, vertex formats, topology, alignments, scissor clamp, vertex layout validation); plain enums, no `webgpu.h` | everywhere, host ctest |
| `include/.../webgpu/shader_registry.hpp`, `src/shader_registry.cpp` | `shader_id` -> WGSL lookup, built-in table, embedded `batched2d.wgsl` | everywhere, host ctest |
| `shaders/batched2d.wgsl` | WGSL twin of the Vulkan batched 2D shaders, embedded as a raw string at configure time | embedded everywhere; validated by naga 30.0.1 when `TESSERA_SHADER_CROSS=ON` |
| `src/webgpu_device.cpp`, `webgpu_resources.cpp`, `webgpu_pipeline.cpp`, `webgpu_command_list.cpp`, `webgpu_internal.hpp`, public `webgpu_device.hpp` | Device, buffer, texture, render target, pipeline, command list against `<webgpu/webgpu.h>` | only `EMSCRIPTEN` or `-DTESSERA_BUILD_WEBGPU=ON` (target `tessera_webgpu`); **unbuilt so far** |

The `tessera_webgpu_mapping` library is cheap and pure, so it builds in normal host configurations;
`tessera_webgpu` is never part of a default build. Tests: `webgpu_mapping` in `tests/unit`, and
`shader_parity` in `tests/gpu/shaders` (see the next section).

## Shader chain and drift guard

Decided in [ADR-049](../../../decisions/ADR-049-backend-extensibility.md) section 1; summary:

- `batched2d.vert`/`.frag` (GLSL) are the single semantic source. Built normally they give
  Vulkan's SPIR-V (unchanged, byte-identical). Built with `-DTESSERA_SHADER_PORTABLE=1` they
  declare the WebGPU interface instead: std140 uniform at binding 0, `texture2D` at 1, `sampler` at
  2, and the texture sampled before the per-primitive branch (WGSL requires implicit-derivative
  sampling in uniform control flow).
- `batched2d.wgsl` stays the authored WebGPU source that is embedded and shipped, so the Wasm build
  needs no translator.
- `shader_parity` (always on, no tools) parses the portable GLSL and the embedded WGSL and fails
  when vertex inputs, varyings (type and flat interpolation), the uniform block members, its size
  versus `Batched2DPushConstants`, or the group-0 bindings differ.
- `-DTESSERA_SHADER_CROSS=ON` (`cmake/TesseraShaderCross.cmake`, `tools/shaders/`) also translates
  the portable SPIR-V to WGSL (spirv-opt + naga), MSL and HLSL (SPIRV-Cross) under
  `<build>/shaders/cross/batched2d/` and registers `shader_cross_*` ctest checks: spirv-val on the
  portable SPIR-V, naga on the generated and the authored WGSL, dxc on the HLSL. The generated WGSL
  is a reference, not shipped; replacing the authored twin with it is a later decision once a
  browser pixel lane compares both.

## Status and what remains

Verified on a Windows host: mapping tables, WGSL embedding and id registry (`ctest -R webgpu_mapping`),
GLSL/WGSL interface parity (`ctest -R shader_parity`), and with `TESSERA_SHADER_CROSS=ON` (Vulkan SDK
1.4.350.0, naga 30.0.1): `batched2d.wgsl` passes naga validation, the portable SPIR-V passes
spirv-val, the generated WGSL passes naga, and the generated HLSL compiles with dxc (SM 6.0).

Not verified (no Emscripten or Dawn headers on the authoring machine): everything in
`tessera_webgpu`. It is written against the current (2025+) `webgpu.h` style (`WGPUStringView`,
`WGPU_*_INIT` macros, `WGPUTexelCopy*`, callback-info structs, `wgpu*AddRef`); adjust to the
emdawnwebgpu revision actually pinned. naga validation of `batched2d.wgsl` does not prove that
Tint (Chrome) accepts it; the browser lane does. MSL output is unvalidated (no Metal toolchain).

To verify in CI: an Emscripten lane (`emcmake cmake -S . -B build/wasm -DTESSERA_BUILD_TESTS=OFF`,
target `tessera_webgpu`) proves it compiles; a browser or Dawn-native lane then runs the batched 2D
scene against the null/lavapipe reference.

Remaining slices: swapchain over the host surface and device-lost callback (`status`/`recover`);
async readback (map + poll) for `Buffer::read`; bind-group caching (today a bind group is created
per draw after a state change, a hot-path allocation to remove); 4-byte-unaligned buffer writes via
staging; manifest-driven parity checks once `manifest.json` exists
([shader-manifest-v1](../../specs/shader-manifest-v1.md)); a CI lane with `TESSERA_SHADER_CROSS=ON`; dependency entries when emdawnwebgpu or Dawn is actually vendored.
