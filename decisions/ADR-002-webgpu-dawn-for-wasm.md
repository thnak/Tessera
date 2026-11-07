# ADR-002: WebGPU (Dawn) for design-time Wasm, separate from the Vulkan backend

- Status: Accepted
- Date: 2026-11-07
- Related: ADR-001, ADR-003, ADR-011

## Context

The design-time viewer must run the real C++ engine in a browser, with no emulator and no
GPU-specific native window. Vulkan cannot run inside WebAssembly; WebGL is legacy and lacks
compute/storage-buffer support. WebGPU is the browser's modern GPU API, and **Dawn** is the
C++ implementation used by Chromium that can be compiled to WebAssembly.

The project will therefore have two GPU backends no matter what. The question is whether to
share code or try to emulate Vulkan on top of WebGPU.

## Decision

1. **Native = Vulkan 1.3** (ADR-001).
2. **WebAssembly design-time = WebGPU via Dawn**, compiled with Emscripten.
3. Share only the **thin RHI** (ADR-003) and the renderer logic above it. Do **not** port the
   Vulkan backend to Wasm and do **not** build a Vulkan-on-WebGPU translation layer in V1.
4. Design the renderer to the **WebGPU feature floor**: no geometry shaders, no push
   constants, conservative uniform/storage buffer limits, explicit bind groups.
5. Keep a single golden-test corpus; each golden must pass on both backends (with the software
   path as the source of truth, ADR-009).

## Consequences

### Positive

- The design-time viewer runs real C++ in a browser with no emulator.
- WebGPU is available on modern Chrome/Edge/Firefox and is the right long-term browser API.
- Dawn is mature, vendorable, and already the reference implementation.

### Negative / risks

- Two backends means two bug surfaces and a parity-testing burden.
- WebGPU limits are lower than Vulkan; renderer features must be designed to the smaller box.
- Dawn adds a large dependency to the Wasm build.
- Browser support and driver bugs are outside our control.

## Alternatives considered

- **WebGL2 for design-time.** Rejected: no compute, weaker resources, legacy direction.
- **Canvas2D.** Rejected: far too slow for the intended live preview and chart workloads.
- **Vulkan-on-WebGPU shim (e.g., a translation layer).** Rejected for V1: immature, adds a
  third moving part, and hides the real WebGPU limits we must design for.
- **Reuse MoltenVK or another portability layer.** Rejected: MoltenVK targets Metal, not
  WebGPU/Wasm.

## Follow-up

- Spike Dawn + Emscripten in Big Step D.
- Establish the minimum WebGPU limits the renderer is designed against.
- Add a parity test that renders the same scene on Vulkan-headless and WebGPU-headless.
