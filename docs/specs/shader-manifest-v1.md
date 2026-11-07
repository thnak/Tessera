# Shader manifest v1

Status: proposed (Big Step B/D). Follow-up to ADR-011.

Tessera keeps a small shader set working on Vulkan (GLSL -> SPIR-V) and WebGPU (WGSL). A
shared manifest describes each shader's interface so the two versions cannot silently diverge.

## 1. File

`tools/shaders/manifest.json` (or `src/rendering/shaders/manifest.json`). One entry per logical
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
        { "name": "solid_quad", "golden": "tests/golden/shaders/batched2d_solid.png" },
        { "name": "sdf_rounded", "golden": "tests/golden/shaders/batched2d_sdf.png" }
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
| `stages` | yes | `vertex`, `fragment`, `compute` (compute optional) |
| `entry` | no | Defaults to `main` per stage |
| `sources.vulkan` | yes | GLSL stage files; compiled by `glslc` |
| `sources.webgpu` | yes | WGSL file(s) |
| `vertexLayout` | vertex stages | Location/format/semantic; must match C++ `Vertex2D` |
| `bindings` | no | Group/binding/type/name; must map to Vulkan descriptor sets and WebGPU bind groups |
| `pushConstants` | no | Vulkan push constants; WebGPU uses a uniform buffer |
| `tests` | no | Golden/smoke tests per shader |

## 4. Compatibility rules

- Shared shaders may not use geometry/tessellation shaders, push constants on WebGPU, or
  features outside the WebGPU floor.
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
  and WGSL reflection where available.

## 5. Build and test

```text
manifest.json
  -> glslc -mfmt=c --target-env=vulkan1.3 -> generated SPIR-V headers
  -> WGSL compile/validate (naga/Tint) -> browser/native WebGPU
  -> reflection check vs manifest
  -> shader smoke/golden tests on lavapipe/software
```

- `glslc` version is pinned.
- Every shader has at least one smoke test; shared shaders have a golden.
- A manifest/source mismatch fails the build.
- Shader changes require a golden update or a documented tolerance.

## 6. Versioning

- Adding optional fields keeps v1.
- Changing binding/layout semantics or removing fields bumps the version and requires a
  migration.
- Runtime shader binaries are generated from the same manifest/source revision.

## 7. Open questions

- Single-source (Slang/Tint/naga) migration; see ADR-011.
- Whether compute shaders are in V1 (optional).
- Reflection for WGSL in the browser build.
- Shader variant matrix (quality/power modes).
