# ADR-011: Shader authoring and cross-compilation (Vulkan SPIR-V + WebGPU WGSL)

- Status: Proposed (GLSL to SPIR-V shaders are used from Big Step B1; the Slang/Tint spike is in [Big Step D](../AGENTS.md#big-steps-group-work-dont-micro-step))
- Date: 2026-11-07
- Related: [ADR-001](ADR-001-vulkan-1-3-baseline.md) (Vulkan 1.3 baseline), [ADR-002](ADR-002-webgpu-dawn-for-wasm.md) (WebGPU/Dawn for WebAssembly), [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin render hardware interface and batched 2D renderer)

## Context

Vulkan consumes SPIR-V (Vulkan shader bytecode); WebGPU consumes WGSL (WebGPU shading language). The two APIs also differ in binding model
(descriptor sets vs bind groups), push constants (Vulkan-only), and feature limits. Tessera
must keep a small shader set working on both backends without turning shader parity into a
permanent maintenance tax.

The [V1](../docs/reference/glossary.md) shader set is small:

- fullscreen triangle (clear / background)
- solid color quad
- SDF (signed distance field) rounded rectangle
- glyph-atlas text
- image / texture quad
- blur, shadow, gradient (after the five shaders listed before it; no release is named)

## Decision

### V1 (accepted direction)

1. Keep the small shader set in **two sources**:
   - GLSL (OpenGL shading language) -> SPIR-V for Vulkan, compiled by `glslc` at build time
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
6. Pin the shader compiler version. Every shader pair gets a [golden](../docs/reference/glossary.md) test.

### Long-term (to be decided in Big Step D)

Evaluate a single-source path and replace the duplication if it is mature:

- **Slang/HLSL -> SPIR-V + WGSL (via naga)**
- **WGSL -> SPIR-V (via Tint)**, if we accept WebGPU as the source-of-truth and give up
  Vulkan-only features
- **DXC (DirectX Shader Compiler) -> SPIR-V + naga -> WGSL**

The spike must compare output parity, toolchain complexity, build time, and debuggability.

## Consequences

### Positive

- Simple, debuggable shaders in V1.
- Each backend uses its native shader language and tooling.
- The manifest makes divergence visible.

### Negative / risks

- Duplication: every shared shader exists twice.
- WGSL restricts what shared shaders can use (no push constants, lower limits).
- A single-source migration (the Long-term options in the Decision section) may force shader rewrites.

## Alternatives considered

- **Slang single-source now.** Attractive, but its WGSL/WebGPU path and build integration are
  not proven enough to make it the V1 dependency. Revisit after the spike.
- **WGSL only + Tint -> SPIR-V.** Rejected for V1: gives up Vulkan-specific control and ties
  native rendering to the WebGPU feature floor.
- **HLSL (High-Level Shading Language) only + DXC.** Rejected for V1: extra toolchain and still needs a WGSL step.
- **Generate shaders from a custom DSL.** Rejected: premature; the shader set is tiny.

## Follow-up

- Write the shader manifest schema and add it to `docs/specs/`.
- Spike Slang and Tint/naga during [Big Step D](../AGENTS.md#big-steps-group-work-dont-micro-step) (design-time preview).
- Record the single-source decision in a superseding ADR if the spike succeeds.
