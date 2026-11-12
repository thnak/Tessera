# Native render surfaces and typed refs

Status: design (proposed with [ADR-050](../../decisions/ADR-050-opt-in-app-framework-layer.md)
section 7; nothing here is implemented). Explains how a component gets a typed ref to a native
render surface that the app's C++ draws into through the RHI. The decisions are normative in
ADR-050; this note carries the detail and the example.

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
