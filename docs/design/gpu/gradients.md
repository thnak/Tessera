# Per-fragment gradients

Status: implemented (issue #617, RHI 1.20, [ADR-076](../../../decisions/ADR-076-per-fragment-gradients.md)).
Owns: how a gradient layer reaches the GPU, the record layout, the fallback rule and the measured
effect. Paint-side grammar lives in [../paint-pass.md](../paint-pass.md) section 13 and
[../background-layers.md](../background-layers.md).

## 1. What changed

Before, one gradient layer was many solid quads: 64 bands for linear, a 32x32 grid for radial, a
64x64 grid for conic (`tessellate_*`, `band_edge` in `paint_sink.hpp`). That cost vertices, was
wrong for oblique angles (a dominant-axis band ignores the other axis), left radial corners
unpainted and could not reproduce a repeating ramp. Now a gradient layer is **one quad** and the
fragment shader evaluates the exact gradient (linear, radial, conic, `repeating-*`, up to 32 stops).
The blend is the straight-alpha blend of a solid quad, and the rounded-corner clip and anti-aliasing
are unchanged. `mask-image` gradients use the same path (`Batched2DSink::fill_*_gradient`).

## 2. How stops reach the shader

A quad carries only a vertex mode (`gradient::kGradientMode` = 8192) and a record index in the
existing `carve_x` attribute. The record lives in the per-frame storage buffer introduced for clip
records ([clip-records.md](clip-records.md), ADR-068), in a gradient area after the clip vertex pool
(std430, `include/tessera/rendering/gpu/gradient_records.hpp`):

| Block | Element | Count | Bytes |
|---|---|---|---|
| `records` | `GradientRecord` { u32 kind, stop_count, first_stop, flags; f32 params[4] } | 512 | 16384 |
| `stop_colors` | vec4, straight RGBA | 2048 | 32768 |
| `stop_offsets` | float, ascending | 2048 | 8192 |

The area starts at byte 53264; the whole buffer is 110608 bytes. Header words 2 and 3 hold the
gradient and stop counts so the upload copies only what was used. Limits: 32 stops per gradient, 512
gradients, 2048 stops per frame.

Params are in **quad-local pixels from the quad centre**, using the interpolated `v_local` the shader
already has, so a gradient follows any transform: linear (dx/span, dy/span, 0.5, 0), radial
(cx, cy, rx, ry), conic (cx, cy, from_turn, 0).

### Why this design

- **Descriptors.** No new binding, set or pipeline: the buffer is set 1 binding 0 (`clip_buffer`),
  already declared by [pipelines.md](pipelines.md) and bound once per frame. A texture-per-gradient
  or a ramp-texture design would add descriptor sets per layer and break batching.
- **Batching.** The batch key (texture, scissor, clip set) is unchanged, so 100 gradients with
  different stops are one draw call. Per-draw uniforms would split batches.
- **Allocation.** The table (`GradientTable`) reserves its vectors once; `add()` never allocates in
  steady state and the table resets each frame.
- **Vertex size.** Putting stops in vertices would multiply 32 stops by 4 vertices; the record is
  referenced by one float.

## 3. Ramp and colour rule

The last stop with offset <= t is found, then the next stop is blended; duplicate offsets: the later
stop wins; both ends are held. Interpolation stays encoded-space sRGB, but **premultiplied** (CSS
Images 3.5.3, what Chromium does), then un-premultiplied for the straight-alpha blend. Opaque and
same-hue ramps are unchanged; a transparent-red to opaque-yellow ramp was 64/255 off Chromium with
straight interpolation and is 1/255 off now. `gradient_math.hpp` is the single CPU definition and the
GLSL (`batched2d.frag`) and WGSL (`batched2d.wgsl`) twins mirror it; Metal gets MSL cross-compiled from
the SPIR-V.

## 4. Capability and fallback

`Device::supports_analytic_gradients()` (RHI 1.20, non-pure, default false): true on Vulkan (when
storage buffers exist), null and WebGPU. Metal keeps false for now, so it stays banded.
`Batched2DRenderer::gradients_supported()` also requires `Batched2DConfig::analytic_gradients` and a
pipeline declaring the storage buffer. `push_gradient` returns false when the capability is missing
or the table is full; the sink then emits the old bands and `gradient_fallbacks()` counts it. A
culled or off-frame gradient returns true before a record is added. The banded code stays only as
this fallback.

## 5. CPU twin

`CpuPaintSink` evaluates the same `pack` + `evaluate` per pixel through `CpuCanvas::fill_rect_shaded`
(pixel-centre rule, float blend), so the CPU twin and the GPU agree within 0-1/255. The CPU sink
still approximates rounded corners by the rect bounds (unchanged).

## 6. Measured effect

| Gradient | Quads before | Quads after | Vertex bytes before | after |
|---|---|---|---|---|
| linear | 64 | 1 | 24576 | 384 |
| radial | 812 | 1 | 311808 | 384 |
| conic | 4096 | 1 | 1572864 | 384 |

Against Chromium 159 (DPR 1, AMD RX 5300M, 15 scenes x 69 samples,
`tests/gpu/batched2d/gradient_chromium_reference.hpp`): the shader is 0-1/255 on every scene. Bands
were 2-8 on masks, 103 on a 63deg linear ramp, 255 on closest-side radial (corners unpainted), 209
on a repeating linear ramp, 40 on repeating radial.

## 7. Tests

- `gradient_math` (unit): ramp equals the old sampler over a 12008-point sweep, premultiplied case,
  pack refusals, layout offsets, capacities.
- `gradient_null`: quads before/after, capability fallback, culling, table limit, batching to one
  draw, per-frame reset.
- `gradient_gpu`: every kind against the CPU twin, transform, rounded clip, 100 gradients in one
  draw, frames in flight.
- `gradient_chromium_gpu`: shader and banded fallback against Chromium per scene.
- `shader_parity`: GLSL/WGSL struct members, capacities, array order and mode agree.
- `layer_mask_gpu`: tolerances tightened to 2/255.
- `cpu_wide_golden`: conics, gradients, layers, masks and radials regenerated; `layer_mask_golden`:
  `tests/golden/gpu/mask_image.png` regenerated on lavapipe (max delta 5 against the banded image, now smooth).

## 8. Not done

- Metal reports no capability; it needs a device test before enabling.
- WGSL is validated by naga only; WebGPU has no runtime test here.
- The inset-shadow and gradient branches share the `v_mode` ladder; the gradient test comes first.
