# Native render surfaces and typed refs

Part of the [ADR-050 decision set](README.md). Section bodies here: `### 7.`. The heading
numbers, levels and order are the original ones, so `ADR-050 section N` still names the same
heading. Each part is a verbatim slice of the former single file; the shared front matter
(status, date, related decisions, terms) is in the [index](README.md).

### 7. Native render surfaces and typed refs (owner decision 4)

[ADR-028](../ADR-028-custom-native-elements.md) covers reusable custom elements from a manifest; this
section adds the per-instance path. Detail, example and comparison:
[`native-render-surfaces.md`](../../docs/design/native-render-surfaces.md).

1. **Ref model.** TSX declares `const viewport = useRef<NativeSurface>(null)` and
   `<NativeSurface ref={viewport} composition="offscreen" redraw="onDemand" />`. `NativeSurface` is
   an intrinsic ref type; any other `T` on that element is a compile error. Codegen lowers the ref
   to a ref cell and the ADR-040 accessor `ref_<name>()`, returning a non-owning
   `NativeSurfaceRef` (node id plus generation) valid from mount to unmount and a checked no-op
   after it. TSX cannot draw; the developer class attaches a renderer in C++ (for example from a
   `useEffect(bind::attach_viewport, [])` member).
2. **One rendering contract.** `SurfaceRenderer` reuses the ADR-028 hooks: attach, resize (pixels
   and DPI scale), visibility, render (RHI `CommandList`, target, damage rect), input (normalized,
   surface-local, through capture/target/bubble), device lost/restored, detach. At most one call
   per frame per surface, so `virtual` is acceptable (appendix).
3. **Ownership and lifetime.** The scene owns the node, the developer class or a service owns the
   renderer, and generated `unmount()` detaches before the developer class is destroyed. GPU
   resources retire through the frames-in-flight queue; device loss drops and later recreates them;
   nothing waits on a fence on the UI thread.
4. **Threading.** V1 renders on the UI thread in the frame pipeline's record step. Worker
   rendering with texture handoff (a ring of at least three targets, completion published through
   the `CompletionQueue`, composited without waiting) follows once the ADR-049 RHI threading rules
   allow it (open question 9).
5. **Dirty rendering and static idle.** A surface redraws only on `request_redraw()`, resize,
   visibility change or device restore, or each frame with `redraw="continuous"` while visible and
   `Active` (a pacer wake that stops on `Hidden`, `Occluded`, `Suspended`). Its rectangle feeds
   damage and scissoring like any node; a static UI with an idle surface presents nothing.
6. **Composition.** `offscreen` (default) is composited by the scene graph as a textured quad, so
   transforms, clips, opacity and z-order apply; ADR-028 `inline` draws into the window target;
   a direct OS sub-surface is deferred (open question 9).
7. **Backend-agnostic seam.** Renderers use only public RHI types and precompiled shaders, so one
   renderer runs on Vulkan, WebGPU (Wasm) and the null backend (structural tests; goldens from
   lavapipe or software). Raw handle interop is a per-backend extension under ADR-049, not V1.
