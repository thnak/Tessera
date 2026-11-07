# ADR-011: Shader authoring and cross-compilation (Vulkan SPIR-V + WebGPU WGSL)

- Status: Proposed (spike in Big Step B/D)
- Date: 2026-11-07
- Related: ADR-001, ADR-002, ADR-003

## Context

Vulkan consumes SPIR-V; WebGPU consumes WGSL. The two APIs also differ in binding model
(descriptor sets vs bind groups), push constants (Vulkan-only), and feature limits. Tessera
must keep a small shader set working on both backends without turning shader parity into a
permanent maintenance tax.

The V1 shader set is small:

- fullscreen triangle (clear / background)
- solid color quad
- SDF rounded rectangle
- glyph-atlas text
- image / texture quad
- (later) blur, shadow, gradient

## Decision

### V1 (accepted direction)

1. Keep the small shader set in **two sources**:
   - GLSL -> SPIR-V for Vulkan, compiled by `glslc` at build time
   - WGSL for WebGPU, authored directly
2. Store them under:
   - `src/rendering/backend/vulkan/shaders/`
   - `src/rendering/backend/webgpu/shaders/`
3. Maintain a **shared shader manifest** describing each shader's inputs, bindings, and
   expected output, so the two versions cannot silently diverge.
4. **Interface rules** for shaders that must be shared:
   - no push constants in shared shaders; use uniforms (Vulkan may use push constants in a
     Vulkan-only optimization pass)
   - binding layouts expressible as both descriptor sets and WebGPU bind groups
   - explicit, documented vertex layouts
5. Generate SPIR-V at build time; never commit `.spv` binaries.
6. Pin the shader compiler version. Every shader pair gets a golden test.

### Long-term (to be decided in Big Step D)

Evaluate a single-source path and replace the duplication if it is mature:

- **Slang/HLSL -> SPIR-V + WGSL (via naga)**
- **WGSL -> SPIR-V (via Tint)**, if we accept WebGPU as the source-of-truth and give up
  Vulkan-only features
- **DXC -> SPIR-V + naga -> WGSL**

The spike must compare output parity, toolchain complexity, build time, and debuggability.

## Consequences

### Positive

- Simple, debuggable shaders in V1.
- Each backend uses its native shader language and tooling.
- The manifest makes divergence visible.

### Negative / risks

- Duplication: every shared shader exists twice.
- WGSL restricts what shared shaders can use (no push constants, lower limits).
- A single-source migration later may force shader rewrites.

## Alternatives considered

- **Slang single-source now.** Attractive, but its WGSL/WebGPU path and build integration are
  not proven enough to make it the V1 dependency. Revisit after the spike.
- **WGSL only + Tint -> SPIR-V.** Rejected for V1: gives up Vulkan-specific control and ties
  native rendering to the WebGPU feature floor.
- **HLSL only + DXC.** Rejected for V1: extra toolchain and still needs a WGSL step.
- **Generate shaders from a custom DSL.** Rejected: premature; the shader set is tiny.

## Follow-up

- Write the shader manifest schema and add it to `docs/specs/`.
- Spike Slang and Tint/naga during Big Step D.
- Record the single-source decision in a superseding ADR if the spike succeeds.
