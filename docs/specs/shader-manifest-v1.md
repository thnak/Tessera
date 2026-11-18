# Shader manifest v1

Status: partly implemented (Big Steps B and D, defined in [`AGENTS.md`](../../AGENTS.md#big-steps-group-work-dont-micro-step)).
Follow-up to [ADR-011](../../decisions/ADR-011-shader-authoring-and-cross-compilation.md) (shader
authoring and cross-compilation) and amended by
[ADR-049](../../decisions/ADR-049-backend-extensibility.md) (shader ids, the portable branch and
opt-in cross-compilation). Sections 1, 2, 3.2, 5 and 9 are implemented and checked by the build;
the rest is the target contract.

Terms: **GLSL** is the OpenGL/Vulkan shading language; **SPIR-V** is Vulkan shader bytecode built
with `glslc`; **WGSL** is the WebGPU shading language; **naga/Tint** are WGSL tools; **SPIRV-Reflect**
reads interface data from SPIR-V; **lavapipe** is the Mesa software Vulkan driver used for
headless tests; **golden** is a deterministic reference image ([`golden-tests-v1.md`](golden-tests-v1.md));
**WebGPU floor** is the minimum feature set every WebGPU implementation must support; **V1** is the
first release scope ([`AGENTS.md`](../../AGENTS.md#non-negotiables), item 8). See also the
[glossary](../reference/glossary.md).

Tessera keeps a small shader set working on Vulkan (GLSL -> SPIR-V) and WebGPU (WGSL). A
shared manifest describes each shader's interface so the two versions cannot silently diverge.

## 1. File

The manifest lives at `tools/shaders/manifest.json`, next to its build-time checker
(`tools/shaders/manifest_check.cpp`): it is a repository build input shared by both backend trees,
not a per-backend source file. One entry per logical shader. Backend-specific sources live under:

```text
src/rendering/backend/vulkan/shaders/
src/rendering/backend/webgpu/shaders/
```

Generated SPIR-V/WGSL is build output; never commit `.spv`.

## 2. Schema

The authored manifest `tools/shaders/manifest.json` is the canonical example (it is validated by
`tessera_shader_manifest_check` before every `glslc` rule, section 5). Its `batched2d` entry:

```json
{
  "manifestVersion": 1,
  "shaders": [
    {
      "name": "batched2d",
      "kind": "element",
      "stages": ["vertex", "fragment"],
      "entry": { "vertex": "main", "fragment": "main" },
      "sources": {
        "vulkan": ["batched2d.vert", "batched2d.frag"],
        "webgpu": ["batched2d.wgsl"]
      },
      "vertexLayout": [
        { "location": 0, "format": "float2", "semantic": "position" },
        { "location": 1, "format": "float2", "semantic": "uv" },
        { "location": 2, "format": "ubyte4norm", "semantic": "color" },
        { "location": 3, "format": "float2", "semantic": "local" },
        { "location": 4, "format": "float2", "semantic": "half_size" },
        { "location": 5, "format": "float", "semantic": "radius" },
        { "location": 6, "format": "float", "semantic": "mode" }
      ],
      "bindings": [
        { "group": 0, "binding": 0, "type": "uniform_buffer", "name": "pc" },
        { "group": 0, "binding": 1, "type": "sampled_texture", "name": "u_texture" },
        { "group": 0, "binding": 2, "type": "sampler", "name": "u_sampler" }
      ],
      "pushConstants": {
        "size": 112,
        "fields": [
          { "name": "viewport", "type": "float2", "offset": 0 },
          { "name": "aa_radius", "type": "float", "offset": 8 },
          { "name": "clip_count", "type": "uint", "offset": 12 },
          { "name": "clips[0].rect", "type": "float4", "offset": 16 },
          { "name": "clips[0].radii", "type": "float4", "offset": 32 },
          { "name": "clips[1].rect", "type": "float4", "offset": 48 },
          { "name": "clips[1].radii", "type": "float4", "offset": 64 },
          { "name": "clips[2].rect", "type": "float4", "offset": 80 },
          { "name": "clips[2].radii", "type": "float4", "offset": 96 }
        ]
      },
      "tests": [
        { "name": "dashboard_smoke" },
        { "name": "dashboard", "golden": "tests/golden/baseline/batched2d/dashboard.png" },
        { "name": "dashboard_shapes",
          "golden": "tests/golden/baseline/batched2d/dashboard_shapes.png" }
      ]
    }
  ]
}
```

The entry records the portable interface, not one backend's descriptor layout: `pushConstants` has
no array field type, so the three Vulkan clip slots are written as their six `float4` members, and
the Vulkan fragment shader's combined `sampler2D` at binding 0 is recorded as the portable
sampled-texture/sampler split at bindings 1 and 2 (section 3, section 8).


## 3. Fields

| Field | Required | Notes |
|---|---|---|
| `manifestVersion` | yes | `1` |
| `name` | yes | Unique logical shader name |
| `kind` | no | `element` (default) or `view-transition-effect`; see section 3.1 |
| `stages` | yes | `vertex`, `fragment`, `compute` (compute optional) |
| `entry` | no | Defaults to `main` per stage |
| `sources.vulkan` | yes | GLSL stage files; compiled by `glslc` |
| `sources.webgpu` | yes (see note) | WGSL file(s); required for WebGPU/designer, optional for Vulkan-only elements |
| `vertexLayout` | vertex stages | Location/format/semantic; must match C++ `Vertex2D` |
| `bindings` | no | Group/binding/type/name; must map to Vulkan descriptor sets and WebGPU bind groups |
| `pushConstants` | no | Vulkan push constants; WebGPU uses a uniform buffer |
| `tests` | yes | At least one smoke test per shader (section 5 rule); golden entries where visuals are deterministic |

Note on `sources.webgpu`: required for any shader used by WebGPU or the designer; optional for
Vulkan-only custom elements (see section 7).

### 3.1 `kind`

- `element`: the shader draws geometry for the batched 2D renderer or a custom element
  ([section 7](#7-custom-element-shaders)). All rules of sections 3 to 5 apply.
- `view-transition-effect`: a full-screen composition shader for a view transition effect
  ([`view-transitions-v1.md`](view-transitions-v1.md) section 7). The manifest entry must have exactly this
  interface; any difference fails the build:

| Part | Required value |
|---|---|
| `stages` | `["vertex", "fragment"]` |
| `vertexLayout` | Omitted or empty. The vertex stage draws one full-screen triangle from the vertex index; there are no vertex buffers |
| `pushConstants` | Omitted (not allowed on WebGPU) |
| `bindings` | `{group 0, binding 0, uniform_buffer, "transition"}`, `{group 0, binding 1, sampler, "samp"}`, `{group 0, binding 2, sampled_texture, "old"}`, `{group 0, binding 3, sampled_texture, "new"}`; no other binding |
| `sources.webgpu` | Required (effects must run in the designer) |
| `tests` | At least one smoke test and one golden |

Layout of the `transition` uniform buffer (64 bytes; identical under GLSL `std140` and WGSL `uniform` rules):

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 4 | float | `progress` |
| 4 | 4 | float | `time` |
| 8 | 8 | float2 | `size` |
| 16 | 8 | float2 | `origin` |
| 24 | 4 | float | `scale` |
| 28 | 4 | uint | `seed` |
| 32 | 32 | float4 x 2 | `params` (8 floats) |

Adding `kind` is an optional field and keeps manifest version 1 (section 6).

### 3.2 Shader id and source table

The **shader id** is the stable name of one logical shader program across every backend
([ADR-049](../../decisions/ADR-049-backend-extensibility.md), decision item 1.2). This section is
normative for the identity rule and for the current id → source mapping.

**Identity rule.** A shader id **is** the manifest `name` of its entry. The two are the same string,
character for character, and this is the only place the mapping is defined:

- `rhi::PipelineDesc::shader_id` carries the id (`include/tessera/rendering/rhi/types.hpp`).
- The C++ constant is a `std::string_view` in
  `include/tessera/rendering/gpu/shader_ids.hpp` whose value equals the manifest `name`.
- A non-SPIR-V backend resolves the id against a table it owns; for WebGPU that table is
  `webgpu::ShaderRegistry` (`src/rendering/backend/webgpu/src/shader_registry.cpp`).
- An id that the manifest does not list is a **configure-time error** once a build rule consumes the
  manifest, and a **pipeline-creation error** at run time (not a crash or an empty pipeline) for a
  backend that cannot resolve it: the WebGPU backend throws `std::invalid_argument` for an empty or
  unknown `shader_id` (`src/rendering/backend/webgpu/src/webgpu_pipeline.cpp`). A
  `shader_id` that is empty means "use the SPIR-V words", which keeps the Vulkan path unchanged.
- **Renaming an id is a manifest migration**, not an edit: it changes the id embedded in compiled
  pipelines and in every backend's table, so the manifest version bumps (section 6) and a migration
  note is required. Never rename an id to make a source file or a C++ symbol "nicer".

**Portable branch.** Where a shader has a source shared with non-Vulkan APIs, the GLSL keeps one
file per stage and selects the interface with `TESSERA_SHADER_PORTABLE` (ADR-049, decision item
1.1): the Vulkan push-constant block and combined image sampler become a std140 uniform buffer at
set 0 binding 0, a `texture2D` at binding 1 and a `sampler` at binding 2. The define is **not** set
in a Vulkan build, so Vulkan SPIR-V is unchanged; `cmake/TesseraShaderCross.cmake` sets it only for
its opt-in cross-compiled outputs. The manifest records the portable interface (section 2); the
selected branch is a source detail, not a manifest field.

**Current table.** Every entry in `tools/shaders/manifest.json` with its sources, constant and
consumers:

| id / `name` | Stages | GLSL sources (`src/rendering/backend/vulkan/shaders/`) | WGSL source (`src/rendering/backend/webgpu/shaders/`) | C++ constant (`shader_ids.hpp`) | Backend table / pipeline desc | Consumer |
|---|---|---|---|---|---|---|
| `batched2d` | vertex, fragment | `batched2d.vert`, `batched2d.frag` | `batched2d.wgsl` | `gpu::kBatched2dShaderId` | `vulkan::batched2d_pipeline_desc()`; `webgpu::builtin_shader_registry()` | `Batched2DRenderer` (`src/app/window_internal.hpp`) |
| `glass_blur` | vertex, fragment | `fullscreen.vert`, `glass_blur.frag` | `glass_blur.wgsl` | `gpu::kGlassBlurShaderId` | `vulkan::glass_blur_pipeline_desc()` | `Batched2DRenderer::draw_backdrop` (glass, `docs/specs/glass-v1.md` section 5) |
| `batched2d_lcd` | vertex, fragment | `batched2d.vert`, `batched2d_lcd.frag` | *(none: `element` with Vulkan sources only, section 3)* | `gpu::kBatched2dLcdShaderId` | `vulkan::batched2d_lcd_pipeline_desc()` (`BlendMode::DualSource`) | none yet: no frame path creates it (issue #471 slice 1, `docs/design/display-aware-rendering.md` section 3.4) |
| `batched2d_array` | vertex, fragment | `batched2d.vert`, `batched2d.frag` (`-DTESSERA_TEXTURE_ARRAY=1`) | *(none: `element` with Vulkan sources only, section 3)* | `gpu::kBatched2dArrayShaderId` | `vulkan::batched2d_array_pipeline_desc()` (`PipelineDesc::texture_array`) | `Batched2DRenderer` when `Device::texture_array_capacity()` is non-zero (RHI 1.7, issue #480 part 2) |

`fullscreen.vert` is a fullscreen triangle from `gl_VertexIndex` with no vertex buffer, so the
manifest's `vertexLayout` is empty; it is a shared source for `glass_blur` only (the Vulkan
`fullscreen.frag` PoC shader is not a manifest entry and has no id).

`batched2d_lcd` is the LCD subpixel variant of `batched2d`: the same vertex interface and
push-constant block, a fragment program that also writes per-channel coverage to output index 1, and
no WGSL source. WGSL has no fragment output index and WebGPU has no SRC1 blend factor, so the entry
is Vulkan-only by construction (section 3's note on optional `sources.webgpu`, section 7's
Vulkan-only element rule); it is compiled by `tessera_add_spv_c` so `glslc` and the manifest check
validate it in every Vulkan build, and the cross lane (which names its two ids explicitly) ignores it.

`batched2d_array` is the descriptor-indexing variant of `batched2d` (RHI 1.7, issue #480 part 2): the
same vertex interface, push-constant block and source files, with `batched2d.frag` compiled a second
time under `-DTESSERA_TEXTURE_ARRAY=1` so its clip/SDF/shadow logic cannot drift from the default
variant - only the sampling line differs (an array of combined image samplers indexed by the
per-vertex `texture_index`, with `GL_EXT_nonuniform_qualifier` because the index is not uniform).
WebGPU has no descriptor indexing, so the entry is Vulkan-only for the same reason `batched2d_lcd` is,
and its WGSL twin declares the `texture_index` attribute but ignores it.

`glass_blur.wgsl` is authored and validated (`scripts/ci/validate_wgsl.py`, the `webgpu.yml` lane)
but is **not yet registered** in `builtin_shader_registry()`: no WebGPU path draws glass, so adding
it there would publish an untested program. Registering it is part of the WebGPU glass work, not a
change to this table.

## 4. Compatibility rules

- Shared shaders may not use geometry/tessellation shaders, push constants on WebGPU, or
  features outside the WebGPU floor. (Shared = a shader with both Vulkan and WebGPU sources.)
- Vulkan-only optimizations (push constants, specialization constants) are allowed only in a
  Vulkan-specific variant with a documented parity fallback.
- Binding layouts must be expressible as both descriptor sets and bind groups.
- Vertex layouts are explicit; no implicit packing.
- WGSL and GLSL must agree on:
  - vertex input locations/formats
  - uniform/binding layouts and types
  - color space and alpha blending
  - clip-space conventions (`Y` flip is backend-specific and documented)
- The manifest is validated at build time against compiled SPIR-V reflection (SPIRV-Reflect)
  and WGSL reflection where available (WGSL reflection in the browser build is an open question,
  section 8).

## 5. Build and test

```text
manifest.json
  -> glslc -mfmt=c --target-env=vulkan1.3 -> generated SPIR-V headers
  -> WGSL compile/validate (naga/Tint) -> browser/native WebGPU
  -> reflection check vs manifest
  -> shader smoke/golden tests on lavapipe/software
```

- `glslc` version is pinned.
- The manifest is validated by `tessera_schema` before `glslc` runs (section 9); a validation
  error fails the build with a stable code and field path. Every `tessera_add_spv_c` rule in
  `src/rendering/backend/vulkan/CMakeLists.txt` runs `tessera_shader_manifest_check` as its first
  command, and `cmake/TesseraShaderCross.cmake` does the same before its portable `glslc`. Both
  rules also depend on the manifest file, so editing it revalidates, and a bad manifest stops the
  build before `glslc` starts.
- Every shader has at least one smoke test; shared shaders have a golden.
- A manifest/source mismatch fails the build.
- Shader changes require a golden update or a documented tolerance (case-file tolerance is the `compare.tolerance` field in
  [`golden-tests-v1.md`](golden-tests-v1.md), which applies to GPU smoke runs only).

### 5.1 Opt-in cross-compilation (`TESSERA_SHADER_CROSS`)

The default build compiles GLSL to SPIR-V with `glslc` and needs no translator. Setting
`TESSERA_SHADER_CROSS=ON` (default OFF, in no preset) adds the `tessera_shader_cross_<id>` targets
from `cmake/TesseraShaderCross.cmake` for contributed backends (ADR-049, decision item 1.3):

- It compiles each stage a second time with `-DTESSERA_SHADER_PORTABLE=1`, runs the manifest check
  first in the same rule, and writes everything under `<build>/shaders/cross/<id>/`. Nothing is
  committed and nothing runs at application run time.
- `glslc` is the only mandatory tool; a missing optional tool **skips** its outputs and its ctest
  checks with a `-- Shader cross: <tool> not found` status line at configure time. `spirv-val`
  validates the portable SPIR-V (always, when found), SPIRV-Cross emits MSL and HLSL, `dxc`
  compiles the HLSL, and `spirv-opt` + naga emit and re-validate WGSL.
- Registered ctest names: `shader_cross_<id>_<stage>_spirv_val`, `..._hlsl_dxc`, `..._wgsl_naga`,
  plus `shader_cross_<id>_authored_wgsl_naga` for the authored WGSL twin.
- The lane that runs it is the `shader-cross` job in `.github/workflows/linux.yml`; the tool setup
  and what it validates are in [`docs/guides/writing-a-backend.md`](../guides/writing-a-backend.md)
  section 5.

**Parity requirement.** A shader built through the portable branch must still pass the same parity
check as the Vulkan build. `shader_parity` (`tests/gpu/shaders/shader_parity_tests.cpp`) runs in
every build — including a default one with no tools — and compares the portable GLSL branch against
the authored WGSL, so a portable-only divergence fails without the cross tools installed. The cross
lane adds the artifact-level checks; it is not what proves parity.

## 6. Versioning

- Adding optional fields keeps v1.
- Changing binding/layout semantics or removing fields bumps the version and requires a
  migration.
- Runtime shader binaries are generated from the same manifest/source revision.

## 7. Custom element shaders

- A custom element references one or more manifest entries by `name` in `ui/elements.json`.
- The shader manifest owns the interface (`stages`, vertex layout, bindings, push constants,
  reflection); the element manifest owns pipeline state (topology, depth/cull/blend, formats,
  samples).
- No runtime shader compilation: an element only selects a prebuilt manifest shader/pipeline.
- Shared/portable elements provide WGSL and respect the WebGPU floor. Vulkan-only elements are
  allowed only when `ui/elements.json` excludes WebGPU; their shader entries may omit WGSL.
- Every custom element shader has a smoke test; deterministic visuals have a golden.
- A missing/invalid shader ref, reflection mismatch, or pipeline/format mismatch fails the
  build, not the first frame.
- See [`custom-elements-v1.md`](custom-elements-v1.md).

## 8. Open questions

- Single-source (Slang/Tint/naga) migration; see [ADR-011](../../decisions/ADR-011-shader-authoring-and-cross-compilation.md).
- Compute shaders are not in V1: the `compute` stage value stays reserved and validation rejects
  it with `compute_stage_unsupported` until a core compute consumer exists (section 9).
- Reflection for WGSL in the browser build.
- Shader variant matrix (quality/power modes).
- Per-backend entry names. The schema has one `entry` map per stage, but the batched2d WGSL twin
  exports `vs_main`/`fs_main` while the Vulkan GLSL exports `main`; the authored manifest records
  the GLSL interface and the WebGPU registry keeps its explicit names. A real per-backend entry
  name (or a rename of the WGSL functions) is a schema decision, not a data fix.
- Literal Vulkan descriptor layout. The default Vulkan fragment shader declares a combined
  `sampler2D` at binding 0, which the binding-type enum cannot express; the manifest records the
  portable split plus the Vulkan push block (section 2). A backend-specific descriptor section
  would be needed to validate a literal Vulkan layout.

## 9. Validation

The manifest is loaded and validated at build time by `tessera_schema`
(`include/tessera/schema/manifest.hpp`). `parse_manifest` validates document text and
`load_manifest_file` reads a file under the byte cap; both return
`std::expected<Manifest, ValidationError>`. Failures are values, never exceptions, and each one
carries:

- `code`: a stable `ValidationErrorCode` (`to_string` is the diagnostic token; codes are
  append-only);
- `path`: the JSON field path of the offending value (`$`, `$.manifestVersion`,
  `$.shaders[0].stages[1]`);
- `detail`: a static description of the rule;
- `offset`: the byte position for JSON syntax/depth errors, otherwise 0.

Rules:

- Every object has a closed key set (root, shader, `sources`, vertex attribute, binding,
  push-constant block, push-constant field, test). Unknown keys, missing required keys, wrong JSON
  types, duplicate JSON keys, trailing content, malformed escapes, invalid UTF-8, and non-finite or
  out-of-range numbers are errors.
- `manifestVersion` must be the integer `1`; anything else is `unsupported_version`.
- `stages` accepts `vertex` and `fragment`. `compute` is reserved and rejected with
  `compute_stage_unsupported` (section 8); unknown tokens and duplicate stages are errors.
- `entry` keys must name declared stages; omitted stages default to `main`.
- Source names are bare file names (no `/`, `\`, drive letter, or `..`); `sources.vulkan` is
  required. Golden paths are project-relative.
- Vertex formats are `float`, `float2`, `float3`, `float4`, `ubyte4norm`, `uint`; locations are
  `0..31` and unique; semantics are unique.
- Binding groups are `0..3`, bindings `0..255`; `(group, binding)` pairs are unique; types are
  `uniform_buffer`, `storage_buffer`, `sampler`, `sampled_texture`, `storage_texture`.
- Push constants: `size` is a multiple of 4 and at most 128; field offsets are 4-byte aligned,
  inside the block, non-overlapping, with unique names; field types are `float`, `int`, `uint`,
  `float2`, `float3`, `float4`. A `view-transition-effect` must not declare them (section 3.1).
- `tests` needs at least one entry and unique names; a `view-transition-effect` needs at least one
  golden. Section 3.1 interface deviations are `view_transition_interface`.
- A shared shader may declare Vulkan push constants: section 3 defines the WebGPU counterpart as a
  uniform buffer, so a `pushConstants` block is not rejected merely because WGSL sources exist.

Limits (the constants live in the header):

| Limit | Value |
|---|---|
| Manifest size | 1,048,576 bytes (1 MiB) |
| JSON nesting | 32 |
| Shaders per manifest | 1024 |
| Stages per shader | 3 |
| Source files per backend | 8 |
| Vertex attributes / bindings / push-constant fields | 32 each |
| Tests per shader | 64 |
| Shader name | 128 bytes |
| Entry point / binding / test name | 64 bytes |
| Golden path | 256 bytes |

The byte cap is enforced before decoding, so an oversized file is rejected without reading it.
The decoder is local to `tessera_schema` (the library is a leaf and links no other project
library), and the public API carries no JSON types.
