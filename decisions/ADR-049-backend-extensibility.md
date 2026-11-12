# ADR-049: Backend extensibility: RHI versioning, shader sources for non-Vulkan backends, community backends

- Status: Proposed
- Date: 2026-11-12
- Related: [ADR-001](ADR-001-vulkan-1-3-baseline.md) (Vulkan baseline),
  [ADR-002](ADR-002-webgpu-dawn-for-wasm.md) (WebGPU/Dawn for Wasm),
  [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin RHI),
  [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (goldens),
  [ADR-011](ADR-011-shader-authoring-and-cross-compilation.md) (shader authoring; amended here),
  [ADR-012](ADR-012-static-linking.md) (static linking),
  [ADR-013](ADR-013-project-license-mit.md) (MIT),
  [ADR-046](ADR-046-web-target-for-demos.md) (web target),
  [`docs/design/gpu/webgpu-backend.md`](../docs/design/gpu/webgpu-backend.md),
  [`docs/specs/shader-manifest-v1.md`](../docs/specs/shader-manifest-v1.md),
  [`docs/specs/golden-tests-v1.md`](../docs/specs/golden-tests-v1.md),
  [`docs/specs/dependencies.md`](../docs/specs/dependencies.md)
- Terms: **backend** = an implementation of `rhi::Device` and its resource, command-list and
  swapchain types. **Tier** = the support level defined in Decision item 4. **Shader id** = a stable
  name for one logical shader program (for example `batched2d`). **Conformance suite** = the
  backend-independent tests defined in Decision item 3.

## Context

The RHI in `include/tessera/rendering/rhi/{types,device,swapchain}.hpp` is the only seam to a GPU API
(ADR-003). Four implementations exist or are in progress:

| Backend | Path | Role |
|---|---|---|
| null | `src/rendering/null/` | deterministic command recording for structure tests |
| Vulkan | `src/rendering/backend/vulkan/` | native, the reference for pixels via lavapipe |
| WebGPU | `src/rendering/backend/webgpu/` | Wasm target (ADR-046); unbuilt on host so far |
| software | `src/rendering/software/` (`tessera_software`, `cpu_canvas.cpp`) | CPU canvas used by CPU goldens |

Three gaps block outside contributors (Metal, D3D12, GLES, embedded) and make the in-tree WebGPU
backend fragile:

1. **Shaders are SPIR-V only.** `rhi::PipelineDesc` holds `vertex_spirv`/`fragment_spirv` pointers
   and sizes. ADR-011 says WGSL is authored directly and kept in step by a shader manifest. The
   WebGPU slice had to add `ShaderRegistry`
   (`src/rendering/backend/webgpu/include/tessera/rendering/webgpu/shader_registry.hpp`), which maps
   the *address* of the SPIR-V array to a WGSL module. That works for one backend inside one
   process, but the key is an incidental pointer, a second non-Vulkan backend would need its own
   registry with the same trick, and the WebGPU design note already flags "needs a future
   `shader_id` field (ADR)".
2. **The RHI has no version.** Nothing states what a backend author may rely on, what changes break
   them, or how long an old signature stays. The first out-of-tree backend would be broken by any
   header edit without warning.
3. **No definition of "a working backend".** Tests under `tests/gpu/{rhi,resources,pipelines,frames,
   backends}` and `tests/parity/` exercise the null and Vulkan paths, but nothing says which of them a
   new backend must pass, nor how pixel tolerance works when a real GPU differs from lavapipe.

There is also a policy gap: it is not written down which parts of the repository are the contract
(and so stay in core) and which a community can own.

## Decision

### 1. Shaders: a `shader_id` on `PipelineDesc`, per-backend authored sources by default, build-time cross-compilation as an optional second source

Options evaluated:

| Option | For | Against |
|---|---|---|
| A. `shader_id` + per-backend authored sources | explicit key, no pointer tricks; each backend uses native tooling; the manifest (shader-manifest-v1) already checks parity; no new toolchain | one source per backend per shader; N backends means N copies of a small shader set |
| B. SPIR-V cross-compiled at build time (SPIRV-Cross to MSL/HLSL/GLSL ES; Naga or Tint to WGSL) | single authored source (GLSL); a new GPU backend costs almost nothing in shaders | adds a build-time tool dependency and its version pin; output quality and binding mapping differ per target; harder to debug; WGSL limits (no push constants) must be enforced upstream anyway |
| C. Both | A for Tier 1, B available to backends that opt in | two paths to document and test |

**Recommendation: C, with A as the normative path and B as an opt-in build step.**

1. `PipelineDesc` gains `std::string_view shader_id` (empty means "use the SPIR-V words", which keeps
   today's callers and the Vulkan backend working unchanged). The SPIR-V fields stay: Vulkan
   consumes them, and for Vulkan they remain the source of truth.
2. A backend resolves `shader_id` against a **shader table it owns**, populated at build time
   (WGSL embedded by CMake today; MSL, HLSL or DXIL for others). `ShaderRegistry` becomes the
   WebGPU implementation of that table, keyed by id; the pointer-identity lookup is removed once
   `batched2d` passes `shader_id`. A shader id that the manifest does not list is a
   configure-time error.
3. Logical shaders and their interface (stages, entry points, bindings, vertex layout, push-constant
   block) stay defined by the shader manifest. Every backend's source for a given id must match the
   manifest. A contributed backend that wants to re-use SPIR-V instead may run SPIRV-Cross (or
   Naga/Tint) **in its own CMake, at build time**, and register the output under the same ids. That
   output is a build artifact, never committed, and its tool version is pinned like `glslc`.
4. The shared-shader interface rules from ADR-011 item 4 (no push constants in shared shaders,
   binding layouts valid as descriptor sets and bind groups, explicit vertex layouts) apply to all
   backends. The Vulkan push-constant path remains a Vulkan-only optimization; backends without
   push constants emulate them with a uniform slice, as the WebGPU backend does.
5. An unknown shader id, or a module that fails the manifest check, makes pipeline creation return
   a value error, not a crash.

Consequences for existing decisions:

- **ADR-011** stays in force for V1 ("two sources, manifest, golden per pair"). Its "Long-term"
  section is narrowed: the single-source spike (Slang, Tint, Naga, DXC) is framed as option B above
  and scoped to *new community backends*, not as a replacement for the Vulkan or WebGPU sources.
  ADR-011's status line gets a pointer to this ADR if this ADR is accepted.
- **No runtime shader compilation** (CLAUDE.md constraint, AGENTS.md non-negotiables) is unchanged
  and applies to option B: translation happens in the build, the product embeds finished shaders.
  A backend whose API can only compile from source at run time (some GLES drivers) may ship
  embedded pre-translated source strings; that is driver compilation, not Tessera translating at
  run time, and must be listed in that backend's README.
- The Wasm build has no `glslc` output; with `shader_id` it no longer needs a stand-in SPIR-V array.

### 2. RHI versioning and compatibility policy

1. `include/tessera/rendering/rhi/version.hpp` defines
   `inline constexpr std::uint32_t kRhiVersionMajor`, `kRhiVersionMinor`, plus
   `kRhiVersion = (major << 16) | minor`. The first published value is `1.0`, set when this ADR is
   accepted and the shader-id change lands.
2. A backend declares the version it implements with a `static_assert` against these constants and
   reports it through `Device::capabilities()` (to be added, minor bump) so the runtime and the
   conformance suite can log it.
3. **Breaking (major bump):** removing or renaming a public RHI type, function or enumerator;
   changing a signature, a default value, an ownership or lifetime rule, an error behavior (for
   example `std::out_of_range` on `Buffer::write`, `SwapchainError` values) or a documented
   threading rule; adding a pure virtual member to an interface; changing the numeric value of an
   enumerator; tightening a documented limit (for example MSAA counts other than 1 and 4).
4. **Non-breaking (minor bump):** adding a type, an enumerator at the end, a defaulted field at the
   end of a descriptor struct, a non-pure virtual whose default is correct for existing backends, or
   an optional capability query.
5. **Deprecation window:** a deprecated RHI entity is marked
   `[[deprecated("use X; removed in RHI N")]]` with a replacement, kept for at least **two minor
   releases and ninety days**, whichever is longer, and removed only in a major bump. The change
   appears in `CHANGELOG.md` and in the "RHI changes" section of the backend guide.
6. Behavioral rules in `docs/design/gpu/rhi.md` are part of the contract. A PR that changes
   `include/tessera/rendering/rhi/**` states its version impact in the description; the docs gate is
   extended to fail when those headers change and `version.hpp` and `CHANGELOG.md` do not
   (follow-up).
7. While Tessera is pre-1.0, a major bump is allowed at any time, but items 3 to 6 still apply so
   contributors get the same signals; the deprecation window is waived only when the PR says why.

### 3. Backend conformance suite

1. The suite is the existing tests moved behind a backend-selectable harness, not a new framework:
   - **Contract tests** (`tests/gpu/rhi`, `resources`, `pipelines`, `frames`, `backends`): device
     creation and loss/recovery (`status`/`recover`), buffer and texture write/read ranges and
     errors, render-target and MSAA resolve, pipeline creation (valid and invalid layouts, unknown
     `shader_id`), command-list state, `submit`/`is_complete`/`wait` semantics, swapchain
     acquire/present/resize values, and the never-block rules.
   - **Pixel tests** (`tests/golden/gpu/`, `tests/parity/`): the batched 2D scenes rendered into a
     render target and read back.
2. The harness takes a backend factory (`--backend <name>`, or a factory registered by an
   out-of-tree CMake) so an out-of-tree backend can link the suite as a CMake package and run it
   without copying tests.
3. **Goldens come only from lavapipe or the CPU rasterizer** (ADR-009). A backend is compared with
   those goldens using a per-channel tolerance and a mismatch-fraction cap, declared per scene in
   `tests/golden/gpu/tolerance.json`. Starting values: max channel delta 2/255 on solid fills and
   8/255 on anti-aliased edges, at most 0.1% of pixels over the delta; to be tuned against the first
   two real backends. Exact match is required only against the backend that produced the golden
   (lavapipe) and the software rasterizer's own goldens. A real GPU never produces or updates a
   golden.
4. **"Passing"** means, for a stated RHI version: all contract tests green; all pixel scenes within
   tolerance; no validation-layer or sanitizer errors in a lane where the backend's API offers them;
   a recorded run log with the RHI version and device name. Skipped tests are allowed only for
   capabilities the backend reports as absent (for example 4x MSAA), and each skip names the
   capability. The suite writes a JSON summary that a PR or release can attach.
5. The determinism rules in `tests/gpu/README.md` apply: no wall clock, network, system fonts or
   random ids.

### 4. Support tiers and what lives where

| Tier | Meaning | CI | Lives |
|---|---|---|---|
| 1 | Maintained by core; releases are blocked on it | Required on every PR (Vulkan on Windows and Linux lavapipe; WebGPU Emscripten compile, plus a Dawn-native or browser pixel lane once it exists; null; software) | in-tree under `src/rendering/` |
| 2 | Community maintained, conformance passing for the current RHI major | Optional lane, run on a schedule and on RHI-touching PRs; failures notify the maintainer and do not block merges | in-tree under `src/rendering/backend/<name>/` only if the maintainer accepts the Tier-2 duties; otherwise out-of-tree |
| 3 | Experimental, may be incomplete | none | out-of-tree, or in-tree behind a CMake option that is OFF by default and in no preset |

Tier-2 duties: a named maintainer; a README stating RHI version, supported platforms and known gaps;
a conformance run attached to each Tessera release candidate; a response within the deprecation
window when the RHI changes. A backend that misses two consecutive release candidates drops to
Tier 3. Promotion to Tier 1 needs core maintainer agreement and a CI lane core can operate.

**Stays in core** (changes need an ADR or spec change):

- the IR spec ([`docs/specs/ir-v1.md`](../docs/specs/ir-v1.md)) and the compiler;
- the RHI interface and its version, the batched 2D renderer and the scene graph;
- the semantics of cascade, layout (Yoga adapter), animation and the fixed-step runtime loop, because
  they define what the UI means independent of a backend;
- the reference backends (null, Vulkan, WebGPU, software), the goldens and the conformance suite;
- the shader manifest and the Vulkan and WebGPU shader sources.

**Community can own:**

- GPU backends: Metal, D3D12, GLES and others, with shader sources under their own `shaders/`
  directory registered by shader id;
- software and embedded backends (framebuffer, e-paper, MCU displays) that implement the RHI or a
  documented subset declared through capabilities;
- HAL platform implementations under `src/hal/*` and `src/platform/*` (window, input, power,
  clipboard, audio, display);
- OS queries, one implementation per OS behind the existing seams, for example
  `system_prefers_reduced_motion()` in `src/app/system_motion.cpp` and the equivalents for contrast,
  color scheme and text scale; core fixes the seam's behavior, the OS-specific code is community
  territory;
- tools, samples, asset converters and editor integrations.

Out-of-tree backends build against an installed Tessera package (headers, `tessera_rhi`, the
conformance harness) and are selected at link time by the application. There is no dynamic plugin
loading in V1 (static linking, ADR-012).

### 5. Licensing and dependency rules for contributed backends

1. Contributed code is MIT (ADR-013) with an SPDX header, and the contributor must have the right to
   license it that way (sign-off as described in CONTRIBUTING.md).
2. Third-party libraries are linked **statically**; GPL and LGPL are rejected (ADR-012). SDK headers
   that are only `#include`d (Metal, D3D12, Vulkan) must have a license that allows redistributing the
   resulting binary. System frameworks and OS DLLs that ship with the OS are not dependencies but are
   listed in the backend README.
3. Any new vendored dependency or build-time tool (SPIRV-Cross, Naga, Tint, DXC, Dawn, a Metal shader
   converter) needs an entry in `docs/specs/dependencies.md` (name, version pin, license, purpose,
   build-time or link-time) and in `THIRD_PARTY_NOTICES.md`, and passes
   `scripts/check_dependencies.py` and `scripts/check_pins.py`. For a build-time tool whose output is
   embedded, the entry states the license position of that output.
4. No network access at configure or build time beyond the pinned fetch mechanism the repository
   already uses; no unpinned tags or branches.
5. Out-of-tree backends are not bound by this section, but a backend listed in the docs index as
   Tier 2 must satisfy it.

## Consequences

Positive:

- A second non-Vulkan backend needs no pointer-identity trick, and the manifest can check each
  source by name.
- Contributors get a version number, a definition of breaking, and a conformance run that says when
  they are done.
- Core keeps the semantics that define the product; GPU and platform breadth can grow without core
  review of each driver.
- The no-runtime-compilation rule survives cross-compilation because translation happens in the
  build.

Negative and risks:

- `PipelineDesc` changes; every backend and the batched 2D renderer's pipeline-creation call site
  is updated once (a minor bump: the field is defaulted and appended).
- The conformance harness refactor touches many existing GPU tests before any new backend benefits.
- Tolerance values are a guess until two different real backends exist; too loose hides bugs, too
  tight makes Tier 2 backends unmaintainable.
- Optional cross-compilation lets backends diverge in text rendering and gradients unless the scene
  set covers those paths.
- Tier 2 maintainers may disappear; the demotion rule limits the cost but does not remove it.

## Alternatives considered

- **Keep `ShaderRegistry` keyed by SPIR-V pointer.** Rejected: it works only because one process
  registers the same address, cannot express a shader without SPIR-V, and silently misses when a
  generated array moves.
- **Cross-compile only (B).** Rejected as the normative path: every non-Vulkan backend would depend
  on one translator's output quality before the shader set is proven, and the interface rules (no
  push constants) are easier to enforce in authored sources.
- **Authored per-backend sources only (A alone).** Rejected as the only path: a one-person Metal or
  D3D12 backend should be able to reuse the GLSL source for the V1 shader set.
- **Dynamic plugin backends (shared libraries).** Rejected for V1: conflicts with static linking, and
  the RHI has no stable C ABI.
- **A C ABI for the RHI.** Deferred: it would be the way to version binary plugins, but it costs a
  second interface to maintain and nothing needs it yet.
- **Backends only in-tree, no tiers.** Rejected: core cannot test hardware it does not own, and every
  RHI change would stall behind unowned code.
- **Real-GPU goldens per backend.** Rejected by ADR-009: nondeterministic across drivers.

## Follow-up

1. Add `shader_id` to `rhi::PipelineDesc`; move `ShaderRegistry` to id keys; update
   `shader-manifest-v1.md` with the id-to-source table and the unknown-id rule.
2. Add `include/tessera/rendering/rhi/version.hpp` and `Device::capabilities()`, and the docs-gate
   check for RHI header changes without a version or changelog entry.
3. Make the GPU test harness backend-selectable; add `tests/golden/gpu/tolerance.json` and the JSON
   conformance summary; document them in [`docs/design/gpu/testing.md`](../docs/design/gpu/testing.md).
4. Write `docs/guides/writing-a-backend.md` (RHI walk-through, shader table, running the suite, tier
   checklist) and add it to `docs/README.md`.
5. Add the tiers table and the in-tree/out-of-tree rule to CONTRIBUTING.md and
   [`docs/design/gpu/backends.md`](../docs/design/gpu/backends.md).
6. Extend `docs/specs/dependencies.md` with a "backend build tools" section (SPIRV-Cross, Naga, Tint,
   DXC), marked not vendored until a backend needs one.
7. Update ADR-011's status line to point here if this ADR is accepted.
8. Ask the user to confirm: tolerance starting values, the 90-day deprecation floor, and whether
   Tier 1 includes WebGPU before the Emscripten lane exists.
