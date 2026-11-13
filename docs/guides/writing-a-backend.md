# Writing a backend

Task guide for adding a GPU backend to Tessera. The contract itself is normative in
[`../design/gpu/rhi.md`](../design/gpu/rhi.md); the decision that created tiers and the conformance
suite is [ADR-049](../../decisions/ADR-049-backend-extensibility.md), with
[ADR-003](../../decisions/ADR-003-thin-rhi-and-batched-2d-renderer.md) for the thin RHI and
[ADR-011](../../decisions/ADR-011-shader-authoring-and-cross-compilation.md) for shader authoring.

Read this before writing code. A backend that does not satisfy its tier's list in §2 is reviewed as
incomplete, not as "mostly done".

## 1. What a backend is

A backend implements `rhi::Device` and the resource, command-list and swapchain types that
`Device` creates. It lives behind the RHI seam (`include/tessera/rendering/rhi/**`) and nothing
above it (scene, paint, batched 2D, app) may learn which backend is running except through
queries the interface already exposes.

- In-tree backends build as `tessera_rhi_<name>` static libraries under
  `src/rendering/backend/<name>/`, plus the null and software paths under `src/rendering/{null,software}/`.
- The public RHI headers must stay free of backend headers. No Vulkan, Dawn, Metal or WebGPU
  header may reach `include/tessera/**`.
- The backend is selected at run time through `rhi::DeviceDesc::backend`; tests must be able to
  point the same suite at a different backend without recompiling.

## 2. Support tiers (what each tier promises)

| Tier | Promise | CI | Where it lives |
|---|---|---|---|
| **1** | Maintained by core; a release is blocked on it. Passes the conformance suite, keeps goldens byte-identical on the reference path, and is validation-clean in debug. | Required on every PR. | In-tree under `src/rendering/` (or `src/rendering/backend/<name>/`). |
| **2** | Community maintained, conformance passing for the current RHI major. | Optional lane, scheduled and on RHI-touching PRs; failures notify the maintainer and do not block merges. | In-tree only if the maintainer accepts the Tier-2 duties; otherwise out-of-tree. |
| **3** | Experimental, may be incomplete. | None. | Out-of-tree, or in-tree behind a CMake option that is OFF by default and in no preset. |

Current assignments:

| Backend | Tier | Notes |
|---|---|---|
| Vulkan 1.3 | 1 | Windows/Linux primary native renderer; lavapipe provides the deterministic CI path. |
| Null | 1 | No GPU; record-and-validate backend used by structural tests. Must reject invalid command order like a validating backend. |
| Software/lavapipe | 1 | Deterministic golden source; never a real GPU framebuffer. |
| WebGPU (Dawn/emdawnwebgpu) | 1 | Promoted 2026-11-13 once `wasm-build` and `wgsl-validation` were green in CI. Browser/Wasm GPU API. |
| MoltenVK | 3 | Deferred; see [ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md). |
| Metal (native) | planned 1 | [ADR-053](../../decisions/ADR-053-native-metal-backend.md); enters at Tier 1 with its own CI lane. |

Tier-2 duties: a named maintainer; a backend README stating the RHI version implemented, supported
platforms and known gaps; a conformance run attached to each release candidate; and a response
inside the RHI deprecation window. Missing two consecutive release candidates drops the backend to
Tier 3. Promotion to Tier 1 needs core maintainer agreement plus a CI lane core can operate.

Do not claim a tier in a PR without the matrix row and the CI evidence it implies.

## 3. What you implement

`rhi::Device` (`include/tessera/rendering/rhi/device.hpp`):

| Member | Notes |
|---|---|
| `backend()` | The backend enum value. |
| `capabilities()` | Reports `RhiCapabilities { backend, rhi_version, timestamps }`. Override it; the default reports the current backend, `kRhiVersion` and your `timestamp_capability()`. |
| `supports_sample_count()` | 1 or 4, per format. Report honestly; never silently fall back to 1 sample. |
| `timestamp_capability()` | The only authority on GPU frame-time samples. A backend without query support returns the default (all false) and the runtime keeps the CPU signal. |
| `status()` / `recover()` | Loss is recoverable state, not an exception. A backend with no recovery path returns `NotSupported`. |
| `create_buffer/texture/render_target/pipeline/command_list` | Creation validates descriptors and throws on invalid input; releases partial allocations on failure. |
| `create_swapchain()` | Returns an error rather than `nullptr` when the surface path is unavailable. |
| `wait_idle()` | Teardown/debug only. Never call it per frame, and never block the UI thread. |
| `memory_statistics()` | Allocator totals; zeros are acceptable on backends without tracking. |

`rhi::CommandList` records `begin/end_render_pass`, binds, viewport/scissor, push constants, draw,
`copy_target_to_buffer`, `blit_target`, and one of `submit()` (blocking, offscreen/readback/goldens)
or `submit_async()` + `is_complete()` + bounded `wait()` (frame path). Implement
`begin_gpu_timestamps`/`gpu_frame_time_ms` only when `timestamp_capability()` says they work.

Parity rules that review checks:

- Invalid command order is rejected the same way on every backend, so null-based structural tests
  stay meaningful.
- Out-of-range `Buffer::write`/`read` throws `std::out_of_range`; a failed mapping throws
  `std::runtime_error`.
- Handles are opaque; mixing objects from different backends is a programming error.
- No hidden blocking: frame code uses `submit_async()`, presentation waits on semaphores, and
  destruction defers until in-flight work completes.
- Determinism: no wall clock, random ids, or driver-dependent defaults inside the RHI contract.

## 4. Declare the RHI version

Every backend declares the contract major it implements and reports it at run time, so a mismatch
is a compile error rather than a mystery at execution:

```cpp
static_assert(rhi::kRhiVersionMajor == 1, "My backend implements RHI 1.x");

rhi::RhiCapabilities MyDevice::capabilities() const {
    return rhi::RhiCapabilities{rhi::Backend::MyBackend, rhi::kRhiVersion,
                               timestamp_capability()};
}
```

`include/tessera/rendering/rhi/version.hpp` holds `kRhiVersionMajor`, `kRhiVersionMinor` and
`kRhiVersion`. A PR that changes `include/tessera/rendering/rhi/**` must also update `version.hpp`
and [`../design/gpu/rhi.md`](../design/gpu/rhi.md); the docs gate (`scripts/check_docs.py`) fails
the change otherwise. Breaking changes bump the major and need an ADR-level decision; additive
compatible changes bump the minor.

Do not name a type `DeviceCapabilities`: `<wingdi.h>` defines that word as a macro
(`DeviceCapabilitiesW`/`DeviceCapabilitiesA`), and the public RHI headers must stay includable from
translation units that also include Windows headers. The reported struct is `RhiCapabilities` for
this reason.

## 5. Shaders

| Backend | Authored source | Build step | Checked by |
|---|---|---|---|
| Vulkan | GLSL | `glslc` (Vulkan SDK) → SPIR-V at build time | shader manifest validator, `shader_parity` |
| WebGPU (Wasm) | WGSL | none (embedded and shipped) | `scripts/ci/validate_wgsl.py` (naga), `shader_parity` |
| Contributed (Tier 2) | GLSL, optional cross-compile | `TESSERA_SHADER_CROSS` (SPIRV-Cross → MSL/HLSL, naga → WGSL, `dxc` validates HLSL) | the cross step itself, output stays in the build directory |

Rules:

- No runtime shader compilation in production, on any backend.
- One logical program has one stable `shader_id`; the manifest ([`../specs/shader-manifest-v1.md`](../specs/shader-manifest-v1.md))
  is the parity contract. A backend that cannot support a feature in the shared set (WGSL has no
  push constants, for example) provides the documented portable branch instead of diverging
  silently.
- Authored sources and generated intermediates are not shipped in release archives unless the
  backend needs them at runtime (WebGPU ships WGSL).

## 6. Wire it into the build

1. Add `src/rendering/backend/<name>/CMakeLists.txt` defining the `tessera_rhi_<name>` static
   library and, when needed, a `TESSERA_BUILD_<NAME>` option (default OFF for anything not Tier 1).
2. Link it from the test and tool targets that should exercise it; the null backend stays the
   default for structural tests.
3. Record every third-party dependency (loader, allocator, translator) in
   [`../specs/dependencies.md`](../specs/dependencies.md) and `THIRD_PARTY_NOTICES.md`, with version
   pin and license. Static linking is mandatory ([ADR-012](../../decisions/ADR-012-static-linking.md));
   system components (Vulkan loader/ICDs, OS frameworks) stay dynamic.
4. Keep the CMake option out of presets for Tier 3.

Build-time tools per backend (the canonical list with licenses is
[`../specs/dependencies.md`](../specs/dependencies.md)):

| Backend | Tools |
|---|---|
| Vulkan | Vulkan SDK 1.4.x: headers, loader, `glslc`, validation layers |
| WebGPU native | Dawn (optional, headless parity) |
| WebGPU Wasm | Emscripten `6.0.13` (`tools/emsdk-version.txt`), emdawnwebgpu port, naga CLI `30.0.1` (`tools/naga-version.txt`) for validation |
| Contributed | optional SPIRV-Cross, `dxc`, naga from the pinned Vulkan SDK |

## 7. Run the suite

Structural and contract checks run on the null backend and, when a device is available, on the real
backend:

```powershell
ctest --preset vs2026-release -R "rhi_|shader_parity|batched2d" --output-on-failure
```

- Use the null backend for command/validation structure, lavapipe/software for pixel goldens, and a
  real GPU only for smoke and tolerance runs. Goldens never come from a real GPU framebuffer
  ([ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md)).
- Real-GPU window and Vulkan tests are skipped by hosted CI; run them locally with
  `scripts/gpu_tests.ps1 -Repeat 3` ([real-GPU lane](real-gpu-ci.md)).
- The backend-selectable harness, per-test tolerances in `tolerance.json` and the JSON conformance
  summary are the shape the suite is moving to (ADR-049, #161); prefer extending that harness over
  adding a parallel ad-hoc test binary.
- A backend README (Tier 2 requirement) states the RHI version implemented, platforms, how to run
  the suite and the known gaps.

## 8. Review checklist

Before requesting review, confirm:

- [ ] `capabilities()` reports the real backend, `kRhiVersion` and the timestamp probe; a
      `static_assert` on `kRhiVersionMajor` is in place.
- [ ] Invalid descriptors and command order fail loudly; valid input from the existing tests works.
- [ ] No per-frame allocation, blocking wait or runtime shader compilation on the frame path.
- [ ] Structural tests pass on the backend; goldens are unchanged on the reference path.
- [ ] Debug runs are validation-clean, or the validation exceptions are documented.
- [ ] Dependencies, licenses and pinned tool versions are recorded.
- [ ] Docs: backend README, tier row in §2 of this guide, and the doc index entry if a new document
      appears.

Tier 1 also needs its CI lane in the same PR that claims Tier 1, and the tier table row must say
which lane proves it.
