# ADR-010: macOS through MoltenVK, deferred from V1

- Status: Accepted (deferred)
- Date: 2026-11-07
- Related: ADR-001, ADR-002

## Context

macOS has no native Vulkan implementation. MoltenVK translates Vulkan to Metal and is the
standard portability layer, but it adds runtime overhead, Metal-specific limits, and an extra
layer to debug. A native Metal backend would be faster but multiplies backend work.

The V1 goal is a focused, high-quality Windows/Linux implementation with a deterministic
headless path. macOS does not have to ship first.

## Decision

1. **macOS is a non-goal for V1.** Windows and Linux are the first-class native targets.
2. When macOS is added, use **MoltenVK behind the existing Vulkan backend**, plus a Cocoa
   platform layer for windowing, input, and lifecycle.
3. Do **not** build a native Metal backend in V1.
4. Keep the platform seam clean: no Win32/X11 assumptions in `core/`, `ui/`, or `rendering/`.
   All OS-specific code lives under `src/platform/` and `src/hal/`.
5. Before committing to MoltenVK, evaluate **Dawn/Metal** as an alternative, because Dawn is
   already a dependency for the WebGPU design-time backend (ADR-002). Choose based on
   performance, feature coverage, and maintenance cost.
6. MoltenVK limitations (no geometry shaders, some format/feature gaps) must be checked
   against the renderer's feature floor before the port starts.

## Consequences

### Positive

- V1 scope stays small and focused.
- The architectural seams remain portable.
- A future macOS port has two viable paths (MoltenVK or Dawn/Metal).

### Negative / risks

- No macOS support in V1.
- MoltenVK may impose limits that require renderer changes.
- macOS testing requires Mac hardware, which affects CI planning.

## Alternatives considered

- **Native Metal backend.** Rejected for V1: another full backend and shader path.
- **Dawn/Metal as the macOS backend.** Kept as a future option; not selected now because it
  ties desktop macOS to the WebGPU feature floor.
- **Drop macOS permanently.** Rejected: unacceptable long-term for a cross-platform framework.
- **Use WebView for macOS.** Rejected: contradicts the native-rendering goal.

## Follow-up

- Revisit after Big Step D (design-time preview) when Dawn is integrated.
- Add a macOS CI runner plan when the port is scheduled.
