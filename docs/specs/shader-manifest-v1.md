# Shader manifest v1

Status: proposed (Big Steps B and D, defined in [`AGENTS.md`](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Follow-up to
[ADR-011](../../decisions/ADR-011-shader-authoring-and-cross-compilation.md) (shader authoring and
cross-compilation).

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

`tools/shaders/manifest.json` or `src/rendering/shaders/manifest.json` (which of the two is the
location is not decided; no decision is recorded). One entry per logical
shader. Backend-specific sources live under:

```text
src/rendering/backend/vulkan/shaders/
src/rendering/backend/webgpu/shaders/
```

Generated SPIR-V/WGSL is build output; never commit `.spv`.

## 2. Schema

```json
{
  "manifestVersion": 1,
  "shaders": [
    {
      "name": "batched2d",
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
        { "group": 0, "binding": 0, "type": "uniform_buffer", "name": "push" },
        { "group": 0, "binding": 1, "type": "sampler", "name": "samp" },
        { "group": 0, "binding": 2, "type": "sampled_texture", "name": "tex" }
      ],
      "pushConstants": {
        "size": 16,
        "fields": [
          { "name": "viewport_width", "type": "float", "offset": 0 },
          { "name": "viewport_height", "type": "float", "offset": 4 },
          { "name": "aa_radius", "type": "float", "offset": 8 },
          { "name": "pad", "type": "float", "offset": 12 }
        ]
      },
      "tests": [
        { "name": "solid_quad", "golden": "tests/golden/gpu/shaders/batched2d_solid.png" },
        { "name": "sdf_rounded", "golden": "tests/golden/gpu/shaders/batched2d_sdf.png" }
      ]
    }
  ]
}
```

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
  error fails the build with a stable code and field path.
- Every shader has at least one smoke test; shared shaders have a golden.
- A manifest/source mismatch fails the build.
- Shader changes require a golden update or a documented tolerance (case-file tolerance is the `compare.tolerance` field in
  [`golden-tests-v1.md`](golden-tests-v1.md), which applies to GPU smoke runs only).

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
- Vertex formats are `float`, `float2`, `float3`, `float4`, `ubyte4norm`; locations are `0..31`
  and unique; semantics are unique.
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
