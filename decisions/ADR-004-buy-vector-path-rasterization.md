# ADR-004: Buy vector path rasterization (ThorVG or Skia Graphite), do not write it in V1

- Status: Accepted (direction); concrete backend chosen after a [Big Step B](../AGENTS.md#big-steps-group-work-dont-micro-step) (core renderer) spike
- Date: 2026-11-07
- Related: [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin render hardware interface and batched 2D renderer), [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (deterministic headless golden tests)

## Context

Vector path rendering looks small and is not: fill rules (nonzero/even-odd), antialiasing,
stroke joins/caps/miters, dashes, gradients, clipping, curve flattening, degenerate geometry,
and numerical robustness. Getting it right across Vulkan and WebGPU is a large, endless
edge-case surface that does not differentiate Tessera's UI model.

The design plan already names ThorVG and Skia as candidate vector backends.

## Decision

1. **Do not implement a path rasterizer in V1.** Delegate vector path fill/stroke to a
   production library behind a `PathRasterizer` interface:
   - first candidate: **ThorVG** (MIT, C++, lightweight, SVG-friendly)
   - fallback: **Skia Graphite** (Vulkan backend; mature, heavier build)
2. Keep the integration behind an interface so the renderer's [scene graph](../docs/reference/glossary.md), clipping, and
   batching do not depend on the chosen library.
3. The library may own path tessellation/rasterization into our render targets; it must not
   own the scene graph, layout, event handling, or the game clock.
4. Run a **[Big Step B](../AGENTS.md#big-steps-group-work-dont-micro-step) spike**:
   - render a representative icon/chart path set on Vulkan-headless
   - measure build size, startup cost, memory, and determinism
   - check license and C++/WebGPU viability
   - then record the concrete choice in a follow-up ADR.
5. WebGPU fallback: if neither library has a usable WebGPU path, render paths through the
   software path for design-time only, and keep the native path on ThorVG/Skia.

## Consequences

### Positive

- Avoids a multi-month correctness pit in an area that is not our differentiator.
- Mature antialiasing, stroke, and gradient behavior from day one.
- Keeps the renderer focused on 2D UI batching and the scene graph.

### Negative / risks

- Adds a sizable dependency and build integration work.
- ThorVG's Vulkan/WebGPU backend maturity must be verified, not assumed.
- Skia's build is heavy and its license (BSD-3-Clause) and binary size must be accepted.
- Two vector backends (native vs design-time) could drift; [golden](../docs/reference/glossary.md) tests must cover both.

## Alternatives considered

- **Write our own tessellator/rasterizer.** Rejected for [V1](../docs/reference/glossary.md): high risk, low differentiation.
- **NanoVG.** Rejected: limited feature set, weak stroke/AA quality for production UI.
- **Blend2D / Cairo.** Rejected: CPU-only, does not fit the GPU pipeline.
- **Vello (Rust).** Rejected: would add a Rust toolchain and FFI (foreign function interface) boundary to a C++ project.

## Follow-up

- Spike ThorVG first; record build size, Vulkan support, and determinism.
- If it fails, spike Skia Graphite.
- Write the `PathRasterizer` interface and a null implementation for tests.
