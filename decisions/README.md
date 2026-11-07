# Architecture Decision Records

Decisions are append-only. Superseding an ADR means writing a new one and marking the old one
`Superseded by ADR-XXX`; do not rewrite history.

Status legend: **Accepted** (in force), **Proposed** (direction chosen, spike pending),
**Superseded** (replaced by a later ADR).

| ADR | Title | Status |
|---|---|---|
| [ADR-001](ADR-001-vulkan-1-3-baseline.md) | Vulkan 1.3 as the native rendering baseline | Accepted |
| [ADR-002](ADR-002-webgpu-dawn-for-wasm.md) | WebGPU (Dawn) for design-time Wasm, separate from Vulkan | Accepted |
| [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) | Thin RHI plus a batched 2D renderer | Accepted |
| [ADR-004](ADR-004-buy-vector-path-rasterization.md) | Buy vector path rasterization (ThorVG or Skia Graphite) | Accepted (backend spike pending) |
| [ADR-005](ADR-005-yoga-layout-adapter.md) | Yoga for Flexbox, with a small Grid subset | Accepted |
| [ADR-006](ADR-006-text-stack.md) | Text stack: FreeType, HarfBuzz, SheenBidi, ICU | Accepted |
| [ADR-007](ADR-007-compiler-ir.md) | A neutral IR between JSX and C++ codegen | Accepted |
| [ADR-008](ADR-008-protected-regions-and-manifest.md) | Protected regions, binding manifest, idempotent merging | Accepted |
| [ADR-009](ADR-009-deterministic-headless-golden-tests.md) | Deterministic headless golden tests on a software path | Accepted |
| [ADR-010](ADR-010-macos-via-moltenvk-deferred.md) | macOS through MoltenVK, deferred from V1 | Accepted (deferred) |
| [ADR-011](ADR-011-shader-authoring-and-cross-compilation.md) | Shader authoring and cross-compilation | Proposed |

## Dependency overview

```
ADR-001 Vulkan 1.3 ─┬─ ADR-003 RHI + 2D renderer ── ADR-004 vector paths
ADR-002 WebGPU/Dawn ─┘        │                     ADR-006 text
                              ├─ ADR-005 layout
                              ├─ ADR-009 goldens
                              └─ ADR-011 shaders

ADR-007 IR ── ADR-008 protected regions
ADR-010 macOS (deferred)
```

## Adding an ADR

1. Copy the structure of an existing ADR: Status, Date, Related, Context, Decision,
   Consequences, Alternatives, Follow-up.
2. Use the next number and a slug: `ADR-012-short-title.md`.
3. Update the table above.
4. If it supersedes an ADR, update the old file's status and link the replacement.
