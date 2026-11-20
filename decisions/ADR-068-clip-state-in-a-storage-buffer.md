# ADR-068: Clip state in a storage buffer: arbitrary clip shapes for `clip-path`

- Status: Proposed (2026-11-20). Owner direction: Tessera is pre-alpha, so the renderer is refactored
  to fit the requirement instead of patched around its current limits.
- Date: 2026-11-20
- Related: [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin RHI, batched 2D renderer),
  [ADR-049](ADR-049-backend-extensibility.md) (RHI versioning, conformance, tiers),
  [ADR-053](ADR-053-native-metal-backend.md) (Metal backend), [ADR-004](ADR-004-buy-vector-path-rasterization.md)
  (vector paths), issue #615 (`clip-path` basic shapes), issue #617 (`mask`, `filter`, conic
  gradient), umbrella #618.
- Terms: a **push constant** is a small block of shader constants recorded into the command stream
  (Metal calls it a constant buffer slot, WebGPU has no native equivalent and the backend emulates it).
  A **storage buffer** is a read-only shader buffer of arbitrary size, bound to a slot. A **clip
  record** is one entry describing one clip shape. A **clip set** is the folded list of clip records
  active for a batch (today's `ClipStateTable::State`). **SDF** is a signed distance function evaluated
  per fragment.

## Context

The batched 2D renderer clips in the fragment shader. `ClipStateTable` folds the active clip stack into
at most `clip::kMaxShaderClips = 3` rounded rectangles and packs them into `Batched2DPushConstants`
(`include/tessera/rendering/gpu/vertex2d.hpp`): 16 bytes of viewport and flags plus three 32-byte
`ClipUniform` slots, 112 bytes in total. Vulkan guarantees only 128 bytes of push constants
(`maxPushConstantsSize`), and the struct has a `static_assert` on its size.

That layout fixes four limits that `clip-path` (#615) runs into:

1. **Shape.** A clip is a rectangle plus four circular corner radii. `clip-path: ellipse()` and
   `inset()` with elliptical radii are not circular-corner rounded rectangles, and `polygon()` has a
   variable number of vertices that cannot sit in a fixed 32-byte slot.
2. **Count.** Three shader clips; deeper nesting is folded into the innermost slot by intersecting
   rectangles, which is conservative but loses rounded-corner information.
3. **Seam.** `rhi::CommandList` has no way to bind a buffer of shader data (it binds vertex buffers,
   textures, a texture array and push constants), and `BufferUsage` has no storage kind.
4. **CPU reference.** `CpuCanvas` clips with one axis-aligned `const rhi::Rect*`, so it cannot produce
   a golden for any shaped clip. Goldens come from lavapipe and the CPU canvas only
   ([AGENTS.md](../AGENTS.md)), so a clip the CPU cannot draw cannot be tested.

The 128-byte figure is the Vulkan minimum, not a cost model. Push constants are recorded per batch when
the clip state changes (`Batched2DRenderer::update_push_constants`), not per quad, and the real cost of
clipping is the batch split a changed clip state forces. Raising the budget to 256 bytes would buy a few
more rounded slots on some devices and would not give `polygon()` a home, so it is not a path forward.

## Decision

Clip shapes move out of push constants into a **per-frame read-only storage buffer**. The push constant
shrinks to viewport data and an index range into that buffer.

### 1. RHI 1.9, additive

Following the precedent of RHI 1.5 to 1.7 (a capability that gates a new command, with a safe default):

- `BufferUsage::Storage`, appended at the end of the enum.
- `CommandList::bind_storage_buffer(std::uint32_t slot, const Buffer& buffer)`. Binding an empty buffer
  or binding without a pipeline that declares the slot is a documented error, as for the texture array.
- `Device::supports_storage_buffers()`, pure virtual so every backend answers (as
  `supports_dual_source_blend()` does). A backend answering `false` keeps the push-constant path with
  its three rounded rectangles; no frame changes there.
- `kRhiVersionMinor` becomes 9, with the history comment in `rhi/version.hpp`.

Backend mapping:

| Backend | Mapping |
|---|---|
| Vulkan | `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` in a long-lived set rewritten in place, the same pattern as `vulkan_texture_array.cpp` (update-after-bind), one buffer per frame in flight so the UI thread never waits on a fence |
| Metal | an argument-buffer or `buffer(n)` slot on the fragment stage |
| WebGPU | a `read-only-storage` binding in the batch bind group (the fragment-stage limit of 8 storage buffers is not a constraint at one buffer) |
| null | records the bind and validates slot and size, so command tests can assert it |

### 2. Clip record format

One buffer per frame, laid out `std430`-compatible, 16-byte aligned:

```text
Header      { u32 record_count; u32 vertex_count; u32 reserved[2]; }
Record[i]   { u32 kind; u32 flags; u32 first_vertex; u32 vertex_count;
              f32 x, y, width, height;          // bounding box in target pixels
              f32 radii[8];                     // per-corner elliptical radii (x, y) pairs
            }                                    // 64 bytes
Vertex[j]   { f32 x, y; }                        // polygon vertices, target pixels
ClipSet[k]  { u32 first_record; u32 count; }     // one per interned state
```

- `kind`: `RoundedRect`, `Ellipse`, `Polygon`. `inset()` and `circle()` lower to `RoundedRect` and
  `Ellipse`, so no kind is needed for them. Elliptical corner radii are in the record from the start,
  which also fixes limit 1 for `inset(… round …)`.
- `flags`: fill rule (`nonzero`, `evenodd`) for polygons.
- `ClipStateTable::State` stops holding `std::array<RoundedRect, 3>` and holds a `[first_record, count]`
  range into a frame-lifetime record list. It still interns, so a batch still stores one state index
  and two draws with the same clip set still share a batch. This is the property that keeps batching
  unchanged.
- The push constant becomes viewport, `aa_radius` and the interned clip-set index, well under 32 bytes.
  `kMaxShaderClips` is retired as a shader limit. `kMaxClipDepth` stays as a bound on the stack and the
  fold-on-overflow rule stays, now at a larger bound (initially 32).

### 3. Shader and precision rules

- The fragment shader multiplies coverage over the clip set. `clip_count == 0` is an early-out, so an
  unclipped draw costs what it costs today.
- Rounded rectangle and ellipse use an SDF with the existing `aa_radius` edge. Polygon uses distance to
  the nearest edge for anti-aliasing and winding number (or crossing parity for `evenodd`) for the sign.
- Limits that are validated, not trusted: at most 256 vertices per polygon and 4096 vertices per frame.
  A clip that exceeds either is replaced by its bounding box, which is conservative (it never paints
  outside the shape) and is counted in diagnostics. The style layer reports the over-limit polygon.
- The scissor fast path is kept. `enclosing_scissor` is computed from each record's bounding box, so a
  shaped clip still narrows the hardware scissor and the shader only refines the edge.

### 4. CPU reference

`CpuCanvas` gains a shaped-clip coverage function that implements the same math as the shader in
float, rounded to 8 bits by a documented rule, replacing the single `const rhi::Rect*` clip argument
over time. The rule is part of the golden contract, and the conformance suite compares backends to the
CPU result within the existing 1 LSB tolerance.

### 5. Migration, behaviour-preserving first

1. RHI 1.9 plus the null and Vulkan implementations plus conformance tests.
2. Metal and WebGPU implementations.
3. `ClipStateTable` and the shader move to the buffer, still with `RoundedRect` records only. No golden
   changes. This slice proves the refactor and is the one that must not move a pixel.
4. Elliptical radii, `Ellipse` records and `CpuCanvas` shaped clip: `circle()`, `ellipse()`, `inset()`.
5. `Polygon` records: `polygon()`.
6. Shape interpolation for `clip-path` animation (same-kind and same-vertex-count), reusing the paint
   interpolation path.

Slices 1 to 3 are a refactor with no user-visible change; #615 lands from slice 4.

## Consequences

- One mechanism covers every clip shape and removes the three-clip ceiling, instead of a second path per
  shape. Later shapes (`path()` if ever admitted, and the clip half of `mask` in #617) add a `kind`, not a
  pipeline.
- The push-constant block shrinks, which also frees room for other per-batch constants.
- Every backend and `CpuCanvas` changes, and a pure-virtual capability means each Tier-1 and Tier-2
  backend must answer it. Tier-2 backends may answer `false` and keep the three-rounded-rectangle path
  until they implement the binding.
- The fragment shader does more work for clipped draws on weak GPUs. The early-out for unclipped draws and
  the retained scissor fast path bound the cost; this is to be measured on real hardware, because
  lavapipe timings are CPU timings and do not predict a GPU.
- Per-frame buffer upload is new work on the frame path. It is one staged copy per frame, sized by the
  validated limits above, with no allocation after warm-up (buffers are grown to a high-water mark
  outside the frame, never on the UI thread's critical path).
- The existing `Batched2DPushConstants` ABI changes, so the WGSL, GLSL and MSL shader sources and the
  parity scenes (`tests/gpu/backends/parity_scenes.cpp`) change together in slice 3.

## Alternatives

| Option | Verdict |
|---|---|
| A. Mask texture rasterized by ThorVG | Rejected as the main path: one render target per shaped element, a re-rasterize per animated frame, blurry edges under zoom, and CPU and GPU golden agreement depends on the rasterizer version. Kept as a possible route for `mask` in #617, where an arbitrary image is the input |
| B. Stencil attachment | Rejected for now: adds a stencil format to every render target and offscreen layer, needs a matching software model in `CpuCanvas`, and does not help the SDF shapes that dominate real use. Revisit if a shape cannot be evaluated analytically |
| C. Raise the push-constant budget to 256 bytes | Rejected: device-dependent (`maxPushConstantsSize` is read nowhere today), buys slots rather than shapes, and gives `polygon()` no home |
| D. Keep push constants, add only `circle()`, `ellipse()` and `inset()` as extra record kinds | Rejected: an ellipse needs elliptical radii and a fixed slot count cannot hold them, so this is the patch this ADR replaces. It would also leave `polygon()` blocked |
| E. Buffer device address (raw GPU pointers) | Rejected: not available on every Tier-1 backend, and it would break the thin-RHI portability that ADR-049 protects |

## Follow-up

- Property-status doc and `stylesheet-v1.md`: `clip-path` row moves from blocked to its slice when the
  slice lands; the #615 note in `AGENTS.md` links this ADR.
- New design note `docs/design/gpu/clip-records.md` with the byte layout and the shader pseudo-code,
  written with slice 1, so this ADR stays the decision and the note stays the rationale.
- Conformance suite: a clip-record scene per kind, run on every backend, with the CPU result as the
  reference.
- Open for owner review: the initial `kMaxClipDepth` of 32, the 256 and 4096 polygon limits, and
  whether Tier-2 backends may answer `supports_storage_buffers() == false` indefinitely or only until
  the next RHI minor.
