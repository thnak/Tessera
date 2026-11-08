# Custom native elements v1

Status: proposed (Big Step B2/C/D). Decision: ADR-028. Builds on
[`ir-v1.md`](ir-v1.md), [`shader-manifest-v1.md`](shader-manifest-v1.md),
[`headless-automation-v1.md`](headless-automation-v1.md), and
[`automation-ids-v1.md`](automation-ids-v1.md).

A custom element is a **native C++ renderer** declared from JSX and resolved at compile time.
It is the escape hatch for custom shaders, high-performance 3D, and element-specific input,
without adding a JavaScript runtime, dynamic plugins, or a second UI model.

## 1. Goals and non-goals

Goals:

- Declare a native-rendered component in JSX with typed props/events.
- Give the implementation a low-level RHI/shader API for custom pipelines and 3D.
- Route input through the normal scene-graph capture/target/bubble pipeline, with element-local
  picking and custom events.
- Reuse elements like any deterministic component: multiple instances, keys, props/state,
  lifecycle, snapshots.
- Fast unit/headless tests without a window; deterministic structural and golden modes.

Non-goals:

- No arbitrary JavaScript in production, no `eval`, no runtime TSX parsing.
- No dynamic plugin ABI, `dlopen`, or runtime shader compilation.
- No raw Vulkan/WebGPU/OS handles in reusable elements.
- No custom element that owns the window, event loop, or whole app.
- Not a replacement for declarative UI; use it only where native rendering is required.

## 2. Declaration surfaces

| Surface | Location | Purpose |
|---|---|---|
| JSX use | `ui/**/*.tsx` | `<CustomElement element="..." />` |
| Element manifest | `ui/elements.json` | id, C++ class, props/events, shaders, render/input/test |
| Native implementation | `ui/elements/<Name>.cpp` | `tessera::element::CustomElement` subclass |
| Shader manifest | `tools/shaders/manifest.json` | shader interface, layouts, tests |
| Generated registry | `generated/elements.g.cpp` | compile-time factory table |
| Generated typings | `generated/elements.d.ts` | typed JSX props/events |

The JSX declaration is the only place the UI author names an element. The manifest is the
contract; the C++ class is the implementation.

## 3. Element manifest

`ui/elements.json`:

```json
{
  "elementsVersion": 1,
  "elements": [
    {
      "id": "Waterfall3D",
      "cppClass": "app::Waterfall3D",
      "source": "ui/elements/Waterfall3D.cpp",
      "props": [
        { "name": "intensity", "type": "float", "default": 0.8, "min": 0.0, "max": 1.0 },
        { "name": "camera", "type": "string", "default": "main" },
        { "name": "palette", "type": "color", "default": "#4cc9f0" }
      ],
      "events": [
        { "name": "select", "payload": { "id": "int", "position": "vec3" } }
      ],
      "input": ["pointer", "wheel", "key", "gamepad"],
      "focusable": true,
      "semantics": { "role": "image", "name": "Waterfall 3D" },
      "shaders": ["waterfall3d"],
      "pipeline": {
        "shader": "waterfall3d",
        "topology": "triangle_list",
        "depthTest": true,
        "depthWrite": true,
        "depthCompare": "less",
        "cull": "back",
        "blend": "opaque"
      },
      "render": {
        "composition": "offscreen",
        "mode": "onDemand",
        "colorFormat": "rgba8_unorm",
        "depthFormat": "d32_float",
        "samples": 1,
        "maxPixelSize": [4096, 4096]
      },
      "test": {
        "unit": true,
        "headless": ["structural", "golden"],
        "snapshot": true
      }
    }
  ]
}
```

Fields:

| Field | Required | Notes |
|---|---|---|
| `id` | yes | Unique static id; JSX `element` literal |
| `cppClass` | yes | Fully qualified class deriving `CustomElement` |
| `source` | yes | Project-relative, inside `ui/`; no traversal |
| `props` | no | Typed primitive/vector/resource props |
| `events` | no | Custom event name -> typed payload |
| `input` | no | `pointer`, `wheel`, `key`, `text`, `gamepad` |
| `focusable` | no | Keyboard/focus participation; default `false` |
| `semantics` | no | Static accessibility defaults |
| `shaders` | yes | Manifest shader names used by this element |
| `pipeline` | yes | Pipeline state; shader must be in `shaders` |
| `render` | yes | Composition/format/depth/samples/size cap |
| `test` | no | Unit/headless/golden/snapshot capabilities |
| `platforms` | no | Supported platforms; default all; exclude `webgpu` for Vulkan-only |

Prop types: `bool`, `int`, `float`, `string`, `color`, `vec2`, `vec3`, `vec4`, `mat4`,
`texture`, `buffer`, `node`. No functions, closures, arbitrary objects, or runtime code.

Event payload types: the same scalar/vector types plus arrays of them. Payloads are copied into
the event; no pointers/ownership transfer.

`render.composition`:

- `offscreen` (default) — render into a framework-managed target; composite as a texture.
- `inline` — record into the current frame target at the node viewport with a depth attachment.
  Advanced; see §7.

`render.mode`:

- `onDemand` (default) — render when props/state/layout/input mark the element dirty.
- `continuous` — request frames while visible and active; paused when hidden/occluded/background.

## 4. JSX

```tsx
import { CustomElement } from "tessera";

export function WaterfallPanel() {
  const [selected, setSelected] = useState<number | null>(null);

  return (
    <View className="panel">
      <CustomElement
        element="Waterfall3D"
        style={{ width: 640, height: 360 }}
        props={{ intensity: 0.8, camera: "main", palette: "#4cc9f0" }}
        onPointer={handlePointer}
        onSelect={(event) => setSelected(event.id)}
      />
      <Text>Selected: {selected ?? "none"}</Text>
    </View>
  );
}
```

Rules:

- `element` must be a static string literal matching `ui/elements.json`.
- Props are checked against the manifest schema; no unknown/missing required props.
- Event props are `on` + PascalCase of the manifest event (`select` -> `onSelect`).
- `onPointer`, `onKey`, `onWheel`, `onText`, `onGamepad` are generic input props.
- `CustomElement` is a leaf; use `OverlayHost`/overlay service for overlays, and wrap it in
  `View` for layout children.
- Standard `style`/layout/automation props apply; numeric `uiId` is assigned normally.
- No dynamic component types (`<Component is={...}>`), no element expression, no children.
- No arbitrary object props; use typed vectors/matrices/resource refs.

The compiler generates `generated/elements.d.ts` so TypeScript validates element ids, props,
and event payloads at authoring time.

## 5. Native API

```cpp
namespace tessera::element {

class CustomElement {
public:
    virtual ~CustomElement() = default;

    virtual Result<void> on_attach(CustomElementContext&) = 0;
    virtual Result<void> on_update(const CustomElementUpdate&) = 0;
    virtual Result<void> on_resize(Extent2D pixel_size) = 0;
    virtual Result<void> on_render(CustomElementRenderContext&) = 0;
    virtual EventResult on_event(const ElementEvent&, CustomEventDispatch&) = 0;
    virtual void on_semantics(SemanticsBuilder&) {}
    virtual void on_detach() = 0;

    virtual CustomElementSnapshot snapshot() const { return {}; }
    virtual bool on_test_command(std::string_view, const TestArgs&) { return false; }
};

} // namespace tessera::element
```

Lifecycle:

1. `on_attach` — create/reuse resources; no drawing; may fail.
2. `on_update` — props/state/layout changes; UI thread; no GPU map/read/block.
3. `on_resize` — pixel target changed; recreate after frames in flight.
4. `on_render` — render thread; record commands; no UI mutation; no throw.
5. `on_event` — UI thread; local coordinates; may emit custom events/capture/focus.
6. `on_semantics` — provide accessible semantics.
7. `on_detach` — release resources after frames in flight; no leaks.

`on_update`/`on_event` exceptions are caught by the nearest error boundary; `on_render` must
return an `Error`, not throw. `on_test_command` exists only in debug/headless builds.

### 5.1 Contexts

```cpp
class CustomElementContext {
public:
    rhi::Device& device();
    ElementResourceCache& resources();
    FrameArena& frame_arena();
    const ElementManifest& manifest();
    FrameInfo frame();
};

struct CustomElementUpdate {
    const PropertyBag& props;      // schema-validated
    Extent2D logical_size;
    float scale;
    bool visible;
    bool focused;
    FrameInfo frame;
};

class CustomElementRenderContext {
public:
    rhi::CommandList& commands();
    rhi::TextureView* color();     // offscreen target or frame target
    rhi::TextureView* depth();     // null if no depth
    Rect viewport;                 // logical pixels
    Extent2D pixel_size;
    FrameInfo frame;
    ElementResourceCache& resources();
    void request_render();          // onDemand mode
};
```

- Only the RHI public API is exposed. No Vulkan/WebGPU/OS headers or native handles.
- Resources are created through the cache, not per frame; pipelines are reused.
- `FrameArena` is reset at frame boundaries; no per-frame heap churn after warm-up.
- `FrameInfo` carries monotonic time, frame index, refresh, scale; deterministic in tests.

## 6. Low-level rendering API

Custom elements may use:

- `rhi::Device`, `CommandList`, `Pipeline`, `Buffer`, `Texture`, `TextureView`,
  `RenderTarget`, `Sampler`, `BindGroup`.
- Render passes with color/depth, viewport/scissor, pipeline/bind group, vertex/index buffers.
- `draw`, `draw_indexed`, instanced draws; compute dispatch if the backend supports it.
- Transient frame data via `FrameArena`; persistent resources via `ElementResourceCache`.

Rules:

- No raw backend API, no `vkCmd*`, no `wgpu*`, no OS window/surface handles.
- No runtime shader compilation; shaders are manifest entries built offline.
- No pipeline creation in `on_render` steady state; create/reuse in `on_attach`/on-demand with
  the cache.
- No blocking waits/maps/readbacks in `on_render`; uploads are scheduled, not synchronous.
- No hidden global state; all renderer state is per instance or shared cache data.
- No throwing across render-thread/ABI boundaries; return `Error`.

## 7. Composition and 3D

### 7.1 Offscreen (default)

- Framework creates/reuses a color target (and depth if declared) sized to the node's logical
  bounds × scale, capped by `maxPixelSize`.
- `on_render` receives the target view and viewport; the result is composited into the 2D scene
  as a texture at the node's paint position.
- Preserves clipping/order/isolation; supports transparent 3D over 2D.
- Cost: target memory/resolve/copy; target is cached and retired after frames in flight.
- Resize recreates the target; old target retires after in-flight frames.

### 7.2 Inline (advanced)

- `on_render` records into the current frame target at the node viewport/scissor; a depth
  attachment is provided and cleared.
- Requires: opaque rect, leaf element, no 2D children inside, clip = node rect, no nested inline
  elements, depth clear at start, no blending with prior 3D depth.
- Use for full-bleed 3D/game views; use offscreen for panels with 2D behind/above.
- Renderer preserves painter order: earlier 2D, inline pass, later 2D. Later 2D does not depth
  test; do not draw 2D geometry that should be occluded by 3D.

### 7.3 3D state

- Camera/projection are element props or internal state; matrices are `mat4`.
- Depth format/default compare/cull/blend are declared in `pipeline`; no implicit state.
- MSAA is opt-in via `render.samples`; resolve is RHI-owned.
- Color space/format is explicit (`rgba8_unorm` baseline); HDR/sRGB is capability-gated.
- `continuous` mode is for animation; pause when hidden/occluded/backgrounded.
- `onDemand` is the default; call `request_render()` after internal state changes.

## 8. Input handler

`CustomElement` participates in the normal input pipeline; it does not receive raw OS messages.

```cpp
struct ElementEvent {
    input::InputEvent event;   // normalized pointer/wheel/key/text/gamepad
    float local_x, local_y;    // element-local logical pixels
    std::uint32_t part_id;     // 0 = element; optional hit-test part
};

enum class EventResult { Ignored, Handled, HandledAndStop };

struct CustomEventDispatch {
    void emit(std::string_view name, const EventPayload&);
    void capture_pointer(std::uint32_t pointer_id);
    void release_pointer(std::uint32_t pointer_id);
    void request_focus();
    void prevent_default();
    void stop_propagation();
};
```

Rules:

- Hit testing uses node bounds first; the element may refine with
  `hit_test_part(local_x, local_y) -> uint32_t` for 3D picking/sub-targets.
- Capture/target/bubble still apply; `HandledAndStop` stops propagation.
- Pointer capture/focus use the same runtime state as built-in nodes.
- Keyboard/text/IME only when focused; wheel/gamepad per declared `input`.
- Custom events (`select`, `hover`, `cameraChanged`, ...) are typed by the manifest and
  dispatched to generated JSX bindings; payloads are copied, not owned by the element.
- No global event bus; no direct access to JSX state; no blocking in `on_event`.
- Input scripts in headless tests use `uiId` (+ optional `partId`) and the same path.

## 9. Reuse and lifecycle

- Elements are reusable deterministic components: multiple instances, keys, props/state,
  mount/update/unmount, snapshots.
- The manifest id is unique; the generated registry constructs one instance per node.
- No singleton or static mutable state. Shared pipelines/textures/meshes live in the resource
  cache with refcounts/budgets.
- App state lives in JSX props/state and returns through typed custom events; element-local
  renderer state (camera, hover) must be snapshot-testable.
- Keyed lists preserve element identity; a different `element` id never reuses an instance.
- Lifecycle is child-first on unmount; GPU resources retire after frames in flight.
- An element must be usable in different screens/routes without global initialization order
  assumptions.

## 10. Accessibility

- `semantics` in the manifest provides defaults (role, name, description).
- `on_semantics(SemanticsBuilder&)` can update value/state/actions/children.
- A 3D element must expose a meaningful fallback: name, description, keyboard actions, and
  focus; do not ship a canvas-only interaction with no semantics.
- Custom element semantics participate in the normal semantics tree, focus order, and actions.
- Semantics snapshots are test artifacts; no raw GPU state.

## 11. Fast and headless testing

### 11.1 Unit/fast

- Instantiate the element with a fake context (`element::testing::FakeContext`); no window/GPU.
- Call `on_update`/`on_event`/`on_test_command`; assert `snapshot()` and emitted custom events.
- `on_render` can be skipped or recorded by a fake command list.

### 11.2 Structural headless

- Null RHI records passes/pipelines/draws; no pixels.
- Snapshot includes element id, state, draw counts, pipeline names, vertex/index counts, metrics.
- Assert custom state and event payloads; no GPU required.

### 11.3 Golden

- Software/lavapipe; shaders compiled offline; offscreen target; fixed time/assets/input.
- Compare image + structural report; update only reviewed goldens.
- `continuous` elements use a fixed frame script; no wall clock.

### 11.4 GPU smoke

- Real device; render, submit, readback validation; no baseline updates.
- Device-loss/validation paths tested here.

### 11.5 Test hooks

```json
{ "action": "custom", "uiId": "987654321098765432", "command": "set_camera", "args": { "yaw": 1.2 } }
{ "action": "assertCustom", "uiId": "987654321098765432", "path": "state.camera.yaw", "equals": 1.2 }
{ "action": "assertCustom", "uiId": "987654321098765432", "path": "last_event", "equals": "select" }
```

- `on_test_command` is debug/headless-only; compiled out of release.
- `snapshot` fields: element id, `part_id`, state key/values, draw stats, metrics, last event.
- Trace spans: `element.attach`, `element.update`, `element.event`, `element.render`,
  `element.detach`; carry `uiId`/`componentId`/`nodeId`.

## 12. Security and failure

- Custom elements are trusted native app code compiled into the binary; no untrusted native
  loading and no JS execution.
- `ui/elements.json` is untrusted project input: validate ids, paths, class names, prop/event
  schemas, shader refs, formats, sizes; reject traversal/unknown values.
- Shaders are build-time compiled and reflected against the manifest; no runtime shader source.
- Resource creation validates dimensions/formats/ranges; enforce budgets and caps.
- `on_render` returns `Error`; renderer logs, disables the element, and shows a deterministic
  fallback where possible. No throw across threads/ABI.
- Device loss: detach, release, recreate; no crash; diagnostic. `on_update`/`on_event`
  exceptions are caught by the nearest error boundary.
- No file/network/OS calls in render; test hooks are not shipped in release.
- No secrets/PII in snapshots/traces/goldens.

## 13. Tests

- Manifest validation: unknown id/prop/event/shader, duplicate, traversal, bad formats.
- JSX typing/lowering: literal element, props/events, no children, no dynamic element.
- Generation: deterministic registry/typings; compile-time factory; no dynamic loading.
- Lifecycle: attach/update/resize/render/event/detach order; keyed reuse; no leaks.
- Input: local coords, hit parts, capture, focus, custom events, stop propagation.
- Low-level API: no raw backend headers; pipeline/shader validation; no per-frame creation.
- 3D: offscreen/inline constraints; depth/cull/format; resize/retire; visibility/continuous.
- Accessibility: semantics snapshot/actions/fallback.
- Headless: structural/golden/GPU smoke; custom commands/asserts; determinism.
- Failure: attach/render errors, device loss, boundary catching, fallback.
- Security: path traversal, oversized props/payloads, shader manifest mismatch.

## 14. Open questions

- Inline 3D + 2D depth/order edge cases; overlays above inline use `OverlayHost`; 2D content
  inside inline remains unsupported.
- Indirect draw/compute/ray-tracing scope and capability gates.
- MSAA sample counts/resolve policy on software/lavapipe.
- Raw backend escape hatch for advanced apps (currently forbidden).
- Shader variants/specialization constants and hot reload in dev.
- Part-id stability and automation manifest for sub-targets.
- Custom element state ownership vs JSX props/state; snapshot serialization format.
- WebGPU parity for custom 3D shaders and capabilities.
- Multi-window/multi-display rendering and shared resources.
- GPU timing/audit metrics for custom passes.
