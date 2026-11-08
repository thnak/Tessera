# Native-first boundaries and bridge policy

Status: design. Decision: ADR-024. Applies to runtime, compiler, designer, services, and mobile.

Tessera's production path is **native C++ with direct calls**. "Bridge" means any boundary that
marshals data/control to another runtime: JNI, Objective-C/Swift, JS/Wasm, IPC, or a plugin ABI.
Bridges are allowed only at the edges, never in the UI/render/input/memory hot path.

## 1. Policy

1. **No production JS/React runtime.** Components compile to C++; no virtual DOM, no scripting
   bridge, no per-frame interpreter.
2. **No dynamic plugin ABI.** Static linking (ADR-012); no hot-loadable native modules.
3. **No per-frame serialization.** No JSON/IDL/IPC/FFI in the frame loop.
4. **No cross-language UI ownership.** UI objects never cross a bridge; use values/handles.
5. **Bridges are narrow, typed, versioned, and capability-based.**
6. **OS bridges are unavoidable but thin.** Android JNI/ANativeWindow, iOS ObjC/Metal, Win32,
   X11/Wayland, accessibility/IME/notification APIs.
7. **Build-time adapters are not runtime bridges.** Parser (SWC/oxc/tree-sitter) and shader
   compilers run at build time and are not linked into the app.
8. **Design-time bridge is separate.** Wasm/WebGPU/IPC is for the designer only; production
   never embeds a browser/JS runtime.

```text
Allowed:
  app C++ <-> OS APIs (HAL/platform)
  app C++ <-> accessibility/IME/notification services
  build-time compiler <-> parser/format/shaders
  design-time viewer <-> native host (IPC)

Forbidden:
  production UI <-> JS/React runtime
  production UI <-> scripting engine
  frame loop <-> JSON/IPC/FFI
  UI object <-> JNI/ObjC/Wasm object
  native plugin/module loading
```

## 2. Bridge design rules

- **Ownership:** copy-in/copy-out or explicit shared memory; never borrow a language object past
  the call.
- **Lifetime:** handles/ids, not raw pointers; release global refs/retained objects on shutdown.
- **Threading:** one owner; callbacks marshal to the UI thread; no blocking wait on the UI thread.
- **Errors:** values, not exceptions across FFI/JNI/ObjC/Wasm; never unwind through a callback.
- **Memory:** per-callback scratch, no per-frame allocation/copy; batch; measure bytes/calls.
- **Versioning:** protocol/schema version; reject unknown versions loudly.
- **Security:** validate sizes/paths/payloads; no arbitrary code/FS/network from the viewer.
- **Testing:** contract tests, fuzz malformed messages, leak/refcount tests, no-blocking checks.

## 3. Platform bridges

### Android (JNI / NDK)

- Prefer native handles (`ANativeWindow`, `AInputQueue`, `ASurfaceTexture`) over per-event JNI.
- Cache class/method ids; `RegisterNatives`; avoid `FindClass` in hot paths.
- Bound local refs (`PushLocalFrame`/`PopLocalFrame`); global refs only for long-lived objects.
- Attach/detach worker threads; `JNIEnv` is thread-local.
- Direct `ByteBuffer`/primitive arrays for bulk data; short `GetPrimitiveArrayCritical` sections.
- No Java object graphs for UI state; pass value structs/handles.
- CheckJNI in debug; test local-ref overflow and detach.

### iOS/iPadOS (Objective-C++ / Metal)

- Thin Objective-C++ adapter: ObjC owns the view/controller, C++ owns the engine.
- `@autoreleasepool` per callback; no per-frame ObjC message sends in the render path.
- `CAMetalLayer`/drawable ownership follows the scene/view lifecycle.
- `UITextInput`/`UIAccessibility` are adapters over the native semantics tree, not a second UI.
- ARC for ObjC; explicit policy for C++ holding ObjC references; no unretained dangling pointers.
- No Swift runtime bridge in the hot path; if Swift is used, keep it at the app edge.

### WebAssembly/JS (design-time only)

- Production apps do not include a JS/Wasm runtime.
- The designer uses Emscripten + WebGPU + a versioned IPC bridge.
- Prefer typed arrays/SharedArrayBuffer/WebGPU buffers; no JSON per frame.
- Validate/bound all messages; local-only host bridge; no arbitrary FS/network.
- The custom React reconciler exists only in the designer, never in production.

### Build-time adapters

- JSX/TSX parser (SWC/oxc/tree-sitter), `clang-format`, `glslc`, WGSL tools run at build time.
- They are not linked into the runtime and are not a production bridge.
- Their output is the IR/generated C++/SPIR-V/WGSL; the runtime consumes only native artifacts.

## 4. Memory and bridge interaction

- Bridge scratch is per-callback, bounded, reset after the call.
- No UI object crosses a bridge; no `shared_ptr`/GC object shared across runtimes.
- Bulk data uses direct buffers/shared memory or a copy with an explicit owner.
- Bridge calls/bytes are metrics; a per-frame bridge call is a design smell.
- OOM at a bridge returns an error/degraded result, not a crash.
- See [`memory-model-v1.md`](../specs/memory-model-v1.md) for allocators/lifetimes.

## 5. Threading and events

- OS callbacks enqueue normalized events; they do not call UI code.
- Bridge callbacks marshal to the UI thread; no blocking waits.
- Cancellation/shutdown propagates across the bridge; no callbacks after teardown.
- Deterministic tests inject events; platform bridges are tested separately on devices.

## 6. Security

- Validate every bridge payload (size/type/version/path).
- No arbitrary code execution from project files; the compiler controls the JSX subset.
- No raw filesystem/network access from the designer bridge.
- Redact secrets/PII in bridge logs; crash dumps exclude bridge payloads.
- See [`security.md`](security.md).

## 7. Testing

- Contract tests per bridge: valid/invalid/oversized messages, version mismatch.
- Leak/refcount tests: JNI local/global refs, ObjC retain cycles, Wasm buffers.
- No-blocking checks: bridge callbacks never block the UI thread.
- Fuzz IPC/IDL payloads.
- Device tests for Android/iOS OS bridges; real-device matrix for OEM quirks.
- Verify production build contains no JS/Wasm/React runtime and no dynamic plugin loader.

## 8. Status and open questions

- Android/iOS backends are not implemented; bridges are design-only.
- Designer IPC is not implemented; Wasm/WebGPU is Big Step D.
- Open: JNI vs NativeActivity input path; ObjC++ vs Swift edge; IPC binary format; shared-memory
  zero-copy; exact bridge budgets.
