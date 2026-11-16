# Native render surfaces and typed refs

Status: partially implemented - AF6's surface half (the `hal::NativeSurface` ownership boundary and
`SurfaceRenderer`) landed with
[ADR-050 section 7](../../decisions/ADR-050/native-render-surfaces.md#7-native-render-surfaces-and-typed-refs-owner-decision-4);
the TSX intrinsic, the `useRef<NativeSurface>` lowering and the hook set of section 3 are
still design. Sections 1 and 2 and the hook table describe the unbuilt half. The decisions are
normative in ADR-050; this note carries the detail, the example, and (in section 10) the shape that
shipped.

## 1. Relation to custom elements

[ADR-028](../../decisions/ADR-028-custom-native-elements.md) defines reusable custom elements
registered in `ui/elements.json` with a C++ class per element. A native surface is the
per-instance path: a component places a `NativeSurface` node and its developer class
([ADR-040](../../decisions/ADR-040-generated-base-and-developer-class.md)) renders into it. Both
paths share one rendering contract (the ADR-028 hooks), so there is one way to draw with the RHI
inside the scene.

```tsx
export function ModelViewer() {
  const viewport = useRef<NativeSurface>(null);
  useEffect(bind::attach_viewport, []);         // ADR-040: developer member
  return <NativeSurface ref={viewport} composition="offscreen" redraw="onDemand"
                        className="viewer" />;
}
```

```cpp
class ModelViewer final : public gen::ModelViewerBase<ModelViewer> {
public:
    void attach_viewport() { ref_viewport().attach(renderer_); }   // runs after mount
private:
    SceneRenderer renderer_;   // implements tessera::ui::SurfaceRenderer; owned here
};
```

## 2. Ref model and lowering

`useRef<T>()` with a `ref` prop on an intrinsic is the only way to get a native handle. For native
rendering `T` is `NativeSurface`, an intrinsic ref type known to the compiler; any other `T` on a
`NativeSurface` element is a compile error. Codegen lowers the ref to a generated ref cell and the
ADR-040 accessor `ref_viewport()`, which returns a `NativeSurfaceRef`: a non-owning handle (node id
plus generation) valid from mount to unmount and a checked no-op after unmount. TSX can pass the
ref only to the `ref` prop; it cannot draw. The developer class attaches a renderer in C++.

## 3. Renderer contract

`tessera::ui::SurfaceRenderer` reuses the ADR-028 hook set:

| Hook | When | Notes |
|---|---|---|
| `on_attach(SurfaceContext&)` | renderer attached to a mounted surface | create pipelines and buffers through the RHI |
| `on_resize(size_px, dpi_scale)` | layout or DPI change | recreate size-dependent targets |
| `on_visibility(Visible / Occluded / Hidden)` | window or app visibility change | stop continuous work when not visible |
| `on_render(SurfaceRenderContext&)` | the surface is dirty in this frame | RHI `CommandList`, target, frame index, damage rect |
| `on_input(const InputEvent&)` | normalized event routed to the surface | surface-local coordinates through capture/target/bubble, with capture and focus |
| `on_device_lost()` / `on_device_restored(SurfaceContext&)` | device loss and recovery | drop every resource, then recreate |
| `on_detach()` | unmount or explicit detach | release through retirement |

Calls happen at most once per frame per surface, so a `virtual` interface is acceptable; the
[service-resolve benchmark](service-resolve-benchmark.md) measures a predicted virtual call at the
cost of a non-inlined static call.

## 4. Ownership and lifetime

The scene owns the node; the developer class (or a service) owns the renderer; the node holds a
non-owning pointer that the generated `unmount()` clears before the developer class is destroyed.
GPU resources are created through the RHI by the renderer and retire through the
frames-in-flight retirement queue, never immediately. On device loss the host calls
`on_device_lost()`, shows the surface's placeholder style, and calls `on_device_restored()` when a
device exists again. Unmount during an in-flight frame defers release to retirement; nothing waits
on a fence on the UI thread.

## 5. Threading

V1 renders on the UI thread: `on_render` records into an offscreen target during the frame
pipeline's record step. Worker rendering is a later mode with **texture handoff**: the renderer
draws on a worker into a ring of at least three offscreen targets and publishes a completed target
with its completion value through the `CompletionQueue`; the UI thread composites the newest
completed target and checks completion without waiting, as the window driver already does for
frame slots. It depends on the RHI's cross-thread recording and submission rules (ADR-049).

## 6. Dirty rendering and static idle

A surface redraws only when its renderer calls `request_redraw()` (paint dirty plus damage of the
surface rectangle), on resize, visibility change or device restore, or every frame when
`redraw="continuous"` while the surface is visible and the app is `Active`. Continuous surfaces
register a pacer wake and stop on `Hidden`, `Occluded` or `Suspended`. A static UI with an idle
surface presents nothing. The surface rectangle contributes to damage like any node, and the
redraw plan scissors to it ([`dirty-rendering.md`](dirty-rendering.md)).

## 7. Composition

- `offscreen` (default): the renderer draws into its own target and the scene graph composites it
  as a textured quad, so transforms, clips, opacity, border radius and z-order work like any node.
- `inline` (ADR-028): draws into the window target with explicit ordering constraints, for
  full-bleed opaque views.
- Direct OS sub-surface (overlay plane, child swapchain): platform-specific and deferred.

## 8. Backend-agnostic seam and tests

Renderers see only public RHI types (`Device`, `CommandList`, `Pipeline`, `Buffer`, `Texture`,
`RenderTarget`, `BindGroup`) and precompiled shaders, so the same renderer runs on Vulkan, WebGPU
(Wasm) and the null backend. Raw handle interop (exporting a `VkImage` or `WGPUTexture` to an
external engine) would be a per-backend extension under ADR-049, not part of V1. The null RHI
records the renderer's commands for structural tests; goldens come from lavapipe or the software
rasterizer as for every other node.

## 9. Comparison: eQuantic UI (README level only)

Checked against the project README only (`github.com/eQuantic/equantic-ui`, read 2026-11-12); no
other claims are made. It is a C# framework under MIT, labeled "Development Preview", that renders
through its own GPU engine ("Photon") on Metal and Vulkan with a CPU reference backend, offers a
browser playground, and passes device services (camera, storage, secret store, ...) to components
"by constructor". Comparable to Tessera: an own GPU engine instead of a WebView or Skia, a
normative CPU reference path (Tessera's software and lavapipe goldens), and constructor-style
service access (option A in ADR-050 section 3). Its README does not describe refs or an
embeddable native render surface.

## 10. Implemented shape: the ownership boundary and the renderer (AF6, issue #178)

This is what exists. It is the half of section 7 that makes the rest implementable, and its
acceptance was the ownership claim, so the claim is what the design below is organised around.

### 10.1 `hal::NativeSurface` never owns a window handle

`include/tessera/hal/surface.hpp` is the one type that holds the opaque
`hal::NativeWindowHandle` pair, and it holds it **borrowed**:

| Piece | What it is | Why |
|---|---|---|
| `SurfaceDesc` | physical pixels, DPI scale, `WindowSystem` | a snapshot like `WindowMetrics`; it carries no handle, so a renderer can size a target from it and layout code can read it without touching the OS |
| `SurfaceLifetime` | the owning side: a `shared_ptr<SurfaceLiveness>` with `alive` | the owner (a platform window) holds it; `revoke()` covers an explicit handle destroy before object destruction, and the destructor revokes too |
| `NativeSurface` | `borrow(lifetime, desc, handles, id)` -> an immutable `shared_ptr<const State>` holding a **`weak_ptr`** of the liveness record | the surface observes its owner; it never shares ownership and can never keep a window alive |
| invalid surface | every query answers zeroed values | a stale renderer reads `valid() == false` and zeroes, never a released pointer |

The rule, stated in the header and enforced by the type: **a `NativeSurface` never owns, adopts,
releases or destroys a window handle.** The caller must guarantee that the handles it passes belong
to the owner it borrows from and stay valid exactly as long as the owner reports them; a handle the
platform replaced in place (a resize) needs a fresh `borrow()`. There is no deleter, no teardown
hook and no `std::function` in the type, so "destroying the surface does not destroy the window" is
true by construction rather than by convention.

`tests/unit/hal/native_surface_tests.cpp` proves it in both directions against a fake window that
counts its own teardowns:

- destroying every borrowed surface (including copies) leaves the window's teardown counter at zero,
  its liveness alive and its handle values untouched - and it still lends a new surface afterwards;
- destroying the window makes every copy of the surface report `valid() == false`, zeroed handles,
  zeroed geometry and id `0`, so nothing can read a released pointer;
- `revoke()` invalidates without destroying the owner, and both directions are idempotent.

A surface with `WindowSystem::NoWindow` is valid (a headless host is representable) and a zero-sized
one is not compositable.

### 10.2 `SurfaceRenderer` composites through the RHI, offscreen-capable

`include/tessera/rendering/gpu/surface_renderer.hpp` is shaped like
`src/rendering/backend/webgpu/src/webgpu_canvas_target.cpp`: a render target over an image the
*host* owns, drawn into rather than allocated by presentation code. Here the host-owned half is the
borrowed surface and the destination is a caller-supplied RHI target, so the type never learns
whether the pixels end up in a swapchain image, a test's offscreen target or a canvas.

```cpp
SurfaceRenderer renderer(device, surface, config);      // borrows; never owns
SurfaceFrame frame = renderer.begin_frame(destination); // a slot + the composited target
if (frame) {
    batched.begin(*frame.commands, *frame.target, renderer.target_desc());
    // ...paint...
    batched.end();
    renderer.end_frame();   // composite target -> destination, submit_async
}
```

Design points that matter:

- **the pass is the caller's.** `target_desc()` returns extent, format and clear colour, so
  `Batched2DRenderer` (which owns its pass) draws a surface with no adaptor in between and no way
  for extent/format/clear to drift.
- **`end_frame()` is one composite and one async submit.** An offscreen destination takes
  `copy_target_region` (exact pixels, no filtering - what a golden can compare byte for byte); a
  presentable destination takes the resample `blit_target`, because RHI 1.1 makes a swapchain image
  neither a copy source nor a destination. At scale 1.0 that blit is 1:1.
- **nothing blocks.** A slot whose list has not reported complete is skipped as `SlotBusy`; there is
  no `wait_idle` and no fence wait in the translation unit. Frames that do not present
  (`SlotBusy`, `NoSurface`, `DeviceLost`, `IncompatibleDestination`) are counted separately from
  `composited_frames()`, which is what makes "a static surface presents nothing" assertable.
- **states, not exceptions.** Every refusal is a `SurfaceCompositeStatus` value; an incompatible
  destination is refused rather than silently recoloured.
- **device provenance.** `rendering::vulkan::device_info()` reports
  `VkPhysicalDeviceProperties::deviceType`, `driverID`, driver name and API version, and
  `VulkanDeviceInfo::software()` decides whether the device is a CPU rasterizer. It exists so a
  golden can refuse a wrong renderer instead of trusting `VK_ICD_FILENAMES`, which says what was
  requested, not what answered.

### 10.3 Tests

| Test | Lane | What it proves |
|---|---|---|
| `native_surface` (unit) | every lane | the ownership rule, both directions, on a fake window that counts teardowns |
| `native_surface_renderer` | every lane, null backend | the composited command stream (one pass, the caller's draws, one region copy, one async submit, nothing after it) and the states instead of exceptions |
| `native_surface_golden` | lavapipe lane only (UNIX + a `lvp_icd*` manifest) | the pixel golden of a composited surface, `tests/golden/baseline/surface/native_surface.png` |

The golden refuses a non-software device with the device string in the message (the rule
`scripts/ci/scene_frame_parity.py --write-goldens` applies), renders its frame twice and requires the
two reads to be byte-identical before comparing or rewriting a baseline, and refuses a blank frame.
Provenance of the committed baseline (regenerate with `--update-goldens`, on a software device only):

| | |
|---|---|
| Scene | 64x64 surface, four colour quadrants + centre square + corner marker, `enable_aa=false`, `golden_mode=true`, clear `#111827` |
| Renderer | Mesa llvmpipe / lavapipe, `deviceType 4` (CPU), `driverID 13` (MESA_LLVMPIPE) |
| Written by | `llvmpipe (LLVM 21.1.2, 256 bits)`, API 1.4.318, on Windows |
| Cross-checked | `llvmpipe (LLVM 21.1.8, 256 bits)`, API 1.4.335, on Linux - **same PNG SHA-256** (`237bbaeb...`), i.e. two independent CPU implementations of the same Vulkan version agree bit for bit |

Because the two lanes share no code path beyond the RHI and no build configuration beyond the
preset, that agreement is the strong form of "a lavapipe golden": it means the surface path is not
merely self-consistent on one machine. Both runs are in-process deterministic as well, which the
test asserts every run.

### 10.4 Not implemented here

The TSX intrinsic, the ref lowering, the ADR-028 hook set (`on_attach`/`on_resize`/...), damage and
redraw scheduling, and the `inline` / OS sub-surface composition modes remain design (sections 1-7).
The `NativeSurfaceRef` of section 2 (node id plus generation) is a scene-layer type and would sit
above `hal::NativeSurface`, not replace it.
