# Pipelines, descriptors, and shaders

Status: B1 pipeline path implemented; B2 cache and parity extensions proposed. Shader interface
is owned by [`../../specs/shader-manifest-v1.md`](../../specs/shader-manifest-v1.md); custom
element pipelines by [`../../specs/custom-elements-v1.md`](../../specs/custom-elements-v1.md).

## 1. Pipeline object

A pipeline combines:

- shader stages (SPIR-V for Vulkan, WGSL for WebGPU)
- vertex input layout
- color format/blend
- depth/stencil (custom elements only in V1)
- sample count
- push-constant/uniform layout

No runtime shader compilation. Pipelines are created from build outputs or cached artifacts.

## 2. Cache key

The cache key is explicit and backend-scoped:

```text
backend + shader hashes + entry points + vertex layout + color/depth formats
+ blend + samples + push-constant size + descriptor layout version
```

Rules:

- Equal keys reuse a pipeline; different keys create a new one.
- The key includes the shader manifest/version, not a file path or timestamp.
- Driver/vendor may affect compilation; persistent caches are validated and discarded on mismatch.
- No pointer/address values in the key; it must be stable across runs.
- Pipelines are never created per draw or per frame in steady state.

## 3. Shader modules and manifest

- GLSL stages compile to SPIR-V with a pinned `glslc`; WGSL is compiled/validated for WebGPU.
- The manifest owns stages, entry points, vertex layout, bindings, and push constants.
- SPIR-V reflection is checked against the manifest at build time; mismatch fails the build.
- A shader used by both backends must stay within the WebGPU floor.
- Vulkan-only variants are allowed only for elements that exclude WebGPU, with a documented
  fallback.
- Shader hot reload uses a new generation; old pipelines retire after in-flight frames.
- No runtime shader compilation in production; the designer uses prebuilt/generated shaders.

## 4. Vertex layout

- Locations/formats/offsets are explicit in the manifest and `PipelineDesc`.
- The layout must match the C++ vertex struct (`Vertex2D` for batched 2D).
- No implicit packing, padding, or semantic inference.
- Stride/offset overflow and overlapping attributes are validation errors.
- The same logical layout maps to Vulkan vertex input and WebGPU vertex buffers.

## 5. Fixed state

B1 batched 2D:

- Triangle list, no index buffer, no depth.
- Alpha blend (`src_alpha`, `one_minus_src_alpha`); solid/SDF/textured quads.
- Color format `R8G8B8A8_UNORM`; cull/depth disabled.

Custom elements:

- Declare topology, cull, depth test/write, blend, formats, samples.
- Inline mode requires a depth attachment and opaque/full-bleed rules.
- Offscreen mode composites as a texture and preserves 2D order/transparency.
- Unsupported state fails pipeline creation, not the first draw.

## 6. Descriptors and constants

- B1: one combined image sampler + one uniform/push-constant block per draw where needed.
- Bindings match the manifest (group/binding/type/name); reflection is authoritative.
- Push constants are Vulkan-only in B1; WebGPU uses a uniform buffer with the same logical fields.
- Push-constant size is fixed at pipeline creation; writes must match or fail validation.
- Descriptor sets are frame-slot safe; never mutate a set while a frame using it is in flight.
- Bindless/array descriptors are deferred.

## 7. Pipeline cache lifecycle

| Phase | Behavior |
|---|---|
| B1 | In-memory cache; create on first use; process lifetime |
| B2 | Optional persistent cache; versioned by backend/driver/manifest; discard on mismatch |
| B3 | Multi-window shares cache; retired pipelines wait for in-flight completion |
| Tests | Fixed cache key ordering; golden/smoke use deterministic keys |

- Cache size is bounded; eviction never removes a pipeline in use.
- Corrupt/partial persistent cache is discarded, not trusted.
- Cache creation failure is non-fatal; fall back to in-memory creation.
- Diagnostics record hits/misses/creation time and evictions.

## 8. Failure and fallback

- Shader/manifest mismatch -> build error (not runtime).
- Pipeline creation failure -> fallback pipeline (solid/placeholder) + diagnostic; the app stays
  usable.
- Missing texture/sampler -> 1x1 fallback texture with deterministic color.
- Invalid custom-element pipeline -> element error placeholder; no frame crash.
- Validation errors are fatal in debug/tests.

## 9. Hot reload and tooling

- Hot reload replaces shader/manifest generation; old pipeline retires after in-flight frames.
- Devtools may show pipeline key, shader hashes, creation time, and cache status.
- The designer/WebGPU path must use the same manifest and logical bindings, not a fork.
- No pipeline creation from untrusted runtime source.

## 10. Tests

- Pipeline cache key stability and collision cases.
- Manifest/reflection mismatch fails the build.
- Vertex layout validation.
- Blend/format/depth state creation on null + Vulkan.
- Fallback pipeline on creation failure.
- Hot-reload retirement with fake frame completion.
- Shader smoke/golden tests per manifest entry.

## 11. Deferred/open

- Persistent cache format/versioning across drivers.
- Pipeline libraries/derivatives.
- Bindless/array descriptors.
- Compute pipelines in V1.
- Single-source shader authoring (Slang/Tint/naga).
- WebGPU reflection in the browser build.
