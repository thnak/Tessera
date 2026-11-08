# Custom native elements guide

Status: proposed (Big Steps B2, C and D, defined in [AGENTS.md, Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Normative spec: [`custom-elements-v1.md`](../specs/custom-elements-v1.md).
Decision: [ADR-028](../../decisions/ADR-028-custom-native-elements.md) (custom native elements).

Use a custom element when declarative UI cannot express the visuals: custom shaders, particles,
charts with GPU paths, 3D scenes, video/camera surfaces, or game-like interaction. Everything
else should stay a normal component (a generated UI unit, see [component-model-v1](../specs/component-model-v1.md)).

## 1. When to use

Good fits:

- 3D scene/terrain/point cloud/game view
- custom shader/particle/fluid effect
- GPU chart or large instanced visualization
- element-specific picking (hit-testing inside the element) and input

Bad fits:

- buttons, text, layout, scrolling (use built-in nodes)
- arbitrary JavaScript or runtime plugins (not supported: production has no JS runtime, [ADR-024](../../decisions/ADR-024-native-first-boundaries.md))
- entire app/window ownership
- hiding business state in native globals

## 2. Declare the element

Create `ui/elements.json`:

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
      "input": ["pointer", "wheel", "key"],
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
      "test": { "unit": true, "headless": ["structural", "golden"], "snapshot": true }
    }
  ]
}
```

`element` in JSX is this `id`. The compiler generates `generated/elements.g.cpp` (compile-time factory
registry) and `generated/elements.d.ts` (typed JSX).

## 3. Implement the native element

`ui/elements/Waterfall3D.cpp`:

```cpp
#include <tessera/element/custom_element.hpp>
#include <tessera/rendering/rhi/device.hpp>

namespace app {

class Waterfall3D final : public tessera::element::CustomElement {
public:
    tessera::Result<void> on_attach(tessera::element::CustomElementContext& ctx) override {
        pipeline_ = ctx.resources().pipeline("waterfall3d");
        mesh_ = ctx.resources().mesh("assets/waterfall.mesh");
        return {};
    }

    tessera::Result<void> on_update(const tessera::element::CustomElementUpdate& update) override {
        intensity_ = update.props.get<float>("intensity");
        palette_ = update.props.get<tessera::Color>("palette");
        return {};
    }

    tessera::Result<void> on_resize(tessera::Extent2D pixel_size) override {
        pixel_size_ = pixel_size;
        return {};
    }

    tessera::Result<void> on_render(tessera::element::CustomElementRenderContext& ctx) override {
        auto& cmd = ctx.commands();
        cmd.begin_pass(ctx.color(), ctx.depth(), tessera::ClearColor{});
        cmd.set_viewport(ctx.viewport);
        cmd.set_pipeline(pipeline_);
        cmd.bind_vertex_buffer(mesh_->vertices());
        cmd.bind_index_buffer(mesh_->indices());
        cmd.draw_indexed(mesh_->index_count(), 1, 0, 0, 0);
        cmd.end_pass();
        return {};
    }

    tessera::element::EventResult on_event(
        const tessera::element::ElementEvent& event,
        tessera::element::CustomEventDispatch& dispatch) override {
        if (event.event.type == tessera::input::EventType::PointerDown) {
            auto hit = hit_test(event.local_x, event.local_y);
            if (!hit) return tessera::element::EventResult::Ignored;
            dispatch.capture_pointer(event.event.pointer_id);
            dispatch.emit("select", tessera::element::EventPayload{
                {"id", hit->id}, {"position", hit->position}});
            return tessera::element::EventResult::HandledAndStop;
        }
        return tessera::element::EventResult::Ignored;
    }

    void on_detach() override {
        pipeline_ = nullptr;
        mesh_ = nullptr;
    }

    tessera::element::CustomElementSnapshot snapshot() const override {
        tessera::element::CustomElementSnapshot s;
        s.element_id = "Waterfall3D";
        s.set("state.intensity", intensity_);
        s.set("state.palette", palette_);
        s.set("state.pixel_size", pixel_size_);
        return s;
    }

    bool on_test_command(std::string_view command, const tessera::element::TestArgs& args) override {
        if (command == "set_intensity") {
            intensity_ = args.get<float>("value");
            return true;
        }
        return false;
    }

private:
    tessera::rhi::Pipeline* pipeline_ = nullptr;
    tessera::element::Mesh* mesh_ = nullptr;
    float intensity_ = 0.8f;
    tessera::Color palette_{};
    tessera::Extent2D pixel_size_{};
};

} // namespace app
```

Rules:

- `on_attach`: create/reuse resources; no drawing.
- `on_update`/`on_event`: UI thread; no GPU waits/maps/blocking.
- `on_render`: render thread; record commands through the RHI (render hardware interface, the only GPU seam); no UI mutation; no throw.
- `on_detach`: release GPU resources only after all frames in flight (frames the GPU may still be reading) have completed; no leaks.
- No static mutable state; one instance per node.

## 4. Declare the shader

Add a normal entry to `tools/shaders/manifest.json` (see
[`shader-manifest-v1.md`](../specs/shader-manifest-v1.md)):

```json
{
  "name": "waterfall3d",
  "stages": ["vertex", "fragment"],
  "sources": {
    "vulkan": ["waterfall3d.vert", "waterfall3d.frag"],
    "webgpu": ["waterfall3d.wgsl"]
  },
  "vertexLayout": [
    { "location": 0, "format": "float3", "semantic": "position" },
    { "location": 1, "format": "float3", "semantic": "normal" },
    { "location": 2, "format": "float2", "semantic": "uv" }
  ],
  "bindings": [
    { "group": 0, "binding": 0, "type": "uniform_buffer", "name": "camera" },
    { "group": 0, "binding": 1, "type": "sampled_texture", "name": "heightmap" }
  ],
  "tests": [
    { "name": "waterfall_smoke", "golden": "tests/golden/gpu/elements/waterfall3d.png" }
  ]
}
```

- Shaders are compiled and reflected at build time; no shader source is loaded at runtime.
- Pipeline state lives in `ui/elements.json`; shader interface lives in the shader manifest.
- Vulkan-only elements must be marked unavailable on WebGPU; elements available on both need a WGSL (WebGPU shading language) source that matches the Vulkan shader's output.
- Every shader has a smoke test (renders without validation errors); a golden (deterministic reference image) is required where output is deterministic.

## 5. Use it in JSX

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

- `element` literal only; no dynamic element.
- Props/events are validated by generated typings and the compiler.
- No children; wrap in a `View` for layout, and use `OverlayHost`/overlay service (see [overlays-and-layers-v1](../specs/overlays-and-layers-v1.md)) for overlays.
- `style`/layout/automation props work normally.

## 6. Input and custom events

- The element receives normalized pointer/wheel/key/text/gamepad events only inside its bounds.
- `ElementEvent` provides element-local coordinates and optional `part_id`.
- Implement `hit_test_part(local_x, local_y)` for 3D picking.
- Use `dispatch.capture_pointer`, `request_focus`, `prevent_default`, `stop_propagation`.
- Emit custom events with typed payloads; JSX handlers receive them:
  - manifest event `select` -> JSX `onSelect`
  - payload `{ id: int, position: vec3 }` -> typed event object
- No global event bus and no direct access to JSX state.
- Keyboard/text requires `focusable` and focus; declare `input` categories.

## 7. Reuse

```tsx
<Waterfall3DView intensity={0.4} />
<Waterfall3DView intensity={0.9} />
```

- Multiple instances have separate state/resources; pipelines/meshes are shared through the resource cache.
- Keys preserve identity in lists; an instance is never reused for a different element id.
- App state lives in JSX; native state (camera/hover) must be snapshot-testable.
- Elements must not depend on singleton initialization order, so they work on any route/screen.
- Unmount releases GPU resources after frames in flight.

## 8. Composition choices

| Mode | Use | Cost/constraint |
|---|---|---|
| `offscreen` | panels, 3D over/under 2D, transparency | target/copy/resolve; default |
| `inline` | full-bleed opaque 3D/game view | depth/order constraints; no 2D inside |

- Offscreen target size = node bounds × scale (DPI scale), capped at `maxPixelSize`; recreated on resize.
- Inline uses the frame target + depth, cleared per element; later 2D does not depth test.
- `onDemand` (the default) renders when props/state/layout/input mark the element dirty; `continuous` renders every frame while visible; both pause when the element is hidden.
- Call `request_render()` after internal state changes in `onDemand`.

## 9. Test fast/headless

Unit (fast test of the element class alone, with a fake context and no GPU):

```cpp
element::testing::FakeContext ctx;
app::Waterfall3D el;
REQUIRE(el.on_attach(ctx));
el.on_update({.props = props, .logical_size = {640, 360}, .scale = 1.0f});
auto snap = el.snapshot();
CHECK(snap.get<float>("state.intensity") == 0.8f);
```

Headless script (format defined in [headless-automation-v1](../specs/headless-automation-v1.md); `uiId` is the numeric automation id from [automation-ids-v1](../specs/automation-ids-v1.md)):

```json
{
  "scriptVersion": 1,
  "steps": [
    { "action": "launch" },
    { "action": "waitFor", "uiId": "987654321098765432", "state": "visible" },
    { "action": "custom", "uiId": "987654321098765432", "command": "set_intensity", "args": { "value": 0.4 } },
    { "action": "assertCustom", "uiId": "987654321098765432", "path": "state.intensity", "equals": 0.4 },
    { "action": "click", "uiId": "987654321098765432", "partId": 7 },
    { "action": "assertCustom", "uiId": "987654321098765432", "path": "last_event", "equals": "select" },
    { "action": "screenshot", "path": "out/waterfall.png" },
    { "action": "trace", "path": "out/trace.json" }
  ]
}
```

Modes:

- `structural`: null RHI backend (records commands, no GPU); snapshot draw/pipeline counts; no pixels.
- `golden`: software path (Mesa lavapipe, a software Vulkan driver, or the CPU rasterizer); shader compiled; image + structural report.
- `gpu-smoke`: real device; validation/readback; no baseline update.

Rules:

- Fixed clock/assets/input; no random/wall clock/system fonts.
- `on_test_command` is debug/headless-only.
- Update goldens only after a human has reviewed the image diff.
- Trace spans: `element.attach/update/event/render/detach`.

## 10. Troubleshooting

| Symptom | Check |
|---|---|
| element unknown | `ui/elements.json` id/source; regeneration |
| prop/event type error | manifest schema vs generated `.d.ts` |
| shader missing | shader manifest entry/reflection/pipeline state |
| black/blank | attach/render error, target size, depth/cull, viewport |
| no input | `input`, `focusable`, bounds, hit test, capture |
| ID not found | automation manifest/build; element realized? |
| golden mismatch | shader/AA (anti-aliasing)/DPI/format; run with the same profile and backend as the baseline |
| leak | detach, cache refs, retired frames |

## 11. Status

- Specs/guide only; custom element API, generated registry, and headless hooks are not
  implemented (no source for them exists in `src/` yet).
- Native elements are trusted compile-time code; no dynamic plugins/runtime shaders.
