# ADR-024: Native-first boundaries; no production bridges

- Status: Accepted
- Date: 2026-11-07
- Related: ADR-007, ADR-008, ADR-012, ADR-021, ADR-023

## Context

Tessera compiles a JSX/React design DSL to native C++, uses a Vulkan/WebGPU RHI, and targets
mobile/desktop. It is tempting to reuse a JS runtime, a virtual DOM, a scripting layer, a
per-frame IPC/FFI bridge, or a plugin ABI to connect components, rendering, and platform APIs.
Those choices add cross-language ownership, allocation, synchronization, and failure modes to
the hottest parts of the app and undermine the native performance/determinism goals.

Mobile and design-time still require boundaries: Android JNI/NDK, iOS Objective-C/Metal, and
the Wasm/WebGPU designer. The question is where those boundaries are allowed and what they may
carry.

## Decision

1. **The production UI/render/input/memory path is direct C++.** Generated code calls runtime
   APIs directly; there is no interpreter or virtual DOM in production.
2. **No production JS/React/scripting runtime.** JSX/TSX is a build-time DSL; React semantics
   exist only in the design-time viewer.
3. **No per-frame FFI/IPC/serialization.** No JSON/IDL/FFI in the frame loop. Bridges are
   event/setup boundaries, not frame-loop calls.
4. **No UI objects across a bridge.** Use value structs, stable ids, or explicit shared memory;
   never pass UI/scene/GPU objects to JNI/ObjC/Wasm/JS.
5. **No dynamic plugin ABI or native module loading.** Static linking remains a hard requirement
   (ADR-012).
6. **Bridges are allowed only at:**
   - OS/HAL boundaries (Win32/X11/Wayland, Android JNI/ANativeWindow, iOS ObjC/Metal),
   - accessibility/IME/notification/service APIs,
   - build-time tooling (parser, formatter, shader compiler),
   - the design-time Wasm/WebGPU/IPC viewer.
7. **Bridge rules:** narrow, typed, versioned, capability-based; explicit ownership; errors as
   values; no blocking on the UI thread; bounded scratch memory; contract/leak/fuzz tests.
8. **Mobile bridges are unavoidable but thin.** They adapt surfaces, input, IME, accessibility,
   and lifecycle; they do not own the component tree or render loop.
9. **The design-time bridge is separate.** Production binaries contain no browser/JS runtime;
   the Wasm viewer is development-only.

## Consequences

### Positive

- Native performance and deterministic behavior on the hot path.
- No cross-language ownership/GC hazards in UI/render/input.
- Smaller production binaries and fewer runtime dependencies.
- Clear review rule: a per-frame bridge call is a design smell.
- Mobile still works through narrow, testable OS adapters.

### Negative / risks

- More native adapter code per platform (JNI/ObjC/NDK).
- Mobile IME/accessibility/semantics require real bridge work; not free.
- Design-time and production code paths differ; parity testing is required.
- Some platform features (notifications, permissions, services) still need bridges.

## Alternatives considered

- **Embed a JS/React runtime in production.** Rejected: contradicts generated-native C++ and
  adds GC/ownership/performance nondeterminism.
- **Use a virtual DOM for all updates.** Rejected: production scene graph is retained; generated
  C++ updates nodes directly.
- **Per-frame FFI/IPC to a separate UI process.** Rejected for V1: latency, serialization,
  lifetime, and failure complexity.
- **Dynamic plugin ABI for extensibility.** Rejected: static linking (ADR-012); no stable ABI.
- **Zero bridges on mobile (pure native Activity/UIView).** Not achievable: the OS boundary is
  still a bridge; keep it thin and typed.

## Follow-up

- Enforce in CI: no JS/Wasm/React runtime or plugin loader in production binaries.
- Add bridge contract/leak/fuzz tests; measure bridge calls/bytes.
- Document per-platform bridge ownership in
  [`bridges.md`](../docs/design/bridges.md) and
  [`memory-model-v1.md`](../docs/specs/memory-model-v1.md).
- Review new platform/service integrations against this policy.
