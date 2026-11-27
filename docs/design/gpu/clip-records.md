# Clip records: the per-frame clip buffer

Status: implemented (ADR-068 slice 3, issue #671). The decision is
[ADR-068](../../../decisions/ADR-068-clip-state-in-a-storage-buffer.md); this note is the rationale
and the byte layout the CPU packer and the shaders share.

Terms: a **clip record** is one entry describing one clip shape. A **clip set** is the folded list of
records active for a batch. A **storage buffer** is a read-only shader buffer of arbitrary size bound
to a descriptor set (RHI 1.12, `CommandList::bind_storage_buffer`).

## 1. Why the push-constant block could not hold it

Before this slice the fragment shader received at most three folded rounded rectangles as push
constants: `Batched2DPushConstants` was 112 bytes, of which 96 were three 32-byte `ClipUniform`
slots. That fixed two things at once:

- **Shape.** A slot is a rect plus four circular radii. `ellipse()` and `inset(… round …)` need
  elliptical radii, and `polygon()` has a variable vertex count that no fixed slot holds.
- **Count.** Three slots. Deeper nesting folded into the innermost slot by intersecting rectangles,
  which is conservative but loses rounded-corner information.

Raising the budget to 256 bytes would buy a few more slots on some devices and still give
`polygon()` no home, so the block was not the place to grow.

## 2. The buffer

One `BufferUsage::Storage` buffer per frame in flight, written by the CPU before the frame's draws
are recorded and read by the GPU during them. `ClipBuffer` rotates the slots at frame start, the same
way `VertexArena` rotates its vertex buffers: a slot is reused only once `frames_in_flight` frames
have completed, so a frame never rewrites a buffer a submission still reads.

Layout, `std430`-compatible and 16-byte aligned so the same bytes work as a Vulkan storage buffer and
a WGSL `read-only-storage` binding:

```text
Header      { u32 record_count; u32 reserved[3]; }                      16 bytes
Record[i]   { u32 kind; u32 flags; u32 first_vertex; u32 vertex_count;  64 bytes
              f32 x, y, width, height;
              f32 radii[8]; }
ClipSet[k]  { u32 first_record; u32 count; u32 reserved[2]; }           16 bytes
Vertex[v]   { f32 x, y; }                                                8 bytes
```

The header's second word is the vertex count (slice 5; it was reserved). The vertex pool is the last
area of the clip data, so the record and set offsets slice 3 fixed did not move.

**Gradient area (RHI 1.20, #617).** Header words 2 and 3 are the gradient count and gradient stop
count. After the clip vertex pool (byte 53264) the buffer holds gradient records, stop colours and stop
offsets; see [gradients.md](gradients.md).

- `kind` is `RoundedRect` (0), `Ellipse` (1) or `Polygon` (2). Slice 3 emits `RoundedRect` only;
  slices 4 and 5 add the other two, and the format already has room so they add a kind, not a layout.
- `radii[0..3]` are the corner radii in TL, TR, BR, BL order; `radii[4..7]` are the second component
  of each corner's elliptical pair, zero for a circular corner.
- `flags` carries the polygon fill rule (`nonzero` or `evenodd`); only polygons set it.
- The sets live at a fixed offset after the record area, so the shader's array base is a compile-time
  constant and a set's range is one add.

Capacities are `kMaxClipRecords = 256` and `kMaxClipSets = 256`: 16 KB of records plus 4 KB of sets,
so 20 KB per frame in flight. A frame that interns more than either keeps the sets that fit; the
renderer's scissor still bounds every draw, so dropping a set's shader refinement can only paint
inside the scissor, never outside it.

## 3. What the shader does

The push-constant block shrank from 112 bytes to 16: viewport, `aa_radius` and the interned clip-set
index. The fragment stage reads the set's range and multiplies coverage over its records:

```glsl
float clip_alpha = 1.0;
ClipSetRange clip_set = clip_buffer.sets[pc.clip_set];
for (uint offset = 0u; offset < clip_set.count; ++offset) {
    uint index = clip_set.first_record + offset;
    clip_alpha *= clip_coverage(gl_FragCoord.xy, index);
}
```

`clip_set == 0` is the empty set, so an unclipped draw's loop body never runs. That is the early-out
that keeps an unclipped frame at its old cost, and it is why the renderer needs no special case for
"no clips".

The scissor fast path is unchanged: `enclosing_scissor` is computed from each record's bounding box,
so a shaped clip still narrows the hardware scissor and the shader only refines the edge.

## 4. Batching is unchanged

`ClipStateTable::State` became a set index into the record list, and the table still interns: two
draws with the same folded clip set share one index, so the batch does not split between them. The
batch key is still (texture, scissor, clip set). Slice 3's acceptance is that batch-split counts are
unchanged, and they are.

## 5. The descriptor-set split

The storage buffer is **set 1**, not a replacement for set 0. The consumer that needs it also samples
a texture, so a shader that reads both is the normal case: set 0 is the sampled image (or the
texture array) and set 1 is the clip records.

That split has one consequence worth recording. Vulkan requires two pipeline layouts to be *fully*
compatible for a descriptor-set bind recorded against one to stay valid under the other, and the
push-constant range is part of that comparison. The renderer draws batched2d and glass geometry in
the same pass, so both pipelines now declare the same range (the larger of the two blocks) and the
same set 1. Without that, the glass composite's set-0 bind was recorded against one layout and its
draw used another, which validation reports as an incompatible set.

## 6. What slice 3 does not do

- No new shape: `Ellipse` and `Polygon` records are reserved, not emitted.
- No `CpuCanvas` shaped-clip reference: that is slice 4, with the first shape that needs it.
- No golden changes. Every existing golden is pixel-identical, which is the property that makes this
  slice a refactor rather than a feature.

## 7. Tests

- `gpu_collaborators` pins the table: the empty set, interning, one record per clip, the stack bound
  folding conservatively, and the push-constant block carrying the set index.
- `rounded_clip` pins the pixels: a rounded clip still clips its corners and keeps its interior.
- `shader_parity` pins that the GLSL and WGSL record layouts and the storage binding cannot drift.
- `manifest_build_step` pins that the manifest's push-constant fields and bindings match the GLSL.
- `batched2d`, `batch_split`, `glass_composite_gpu` and `glass_paint_gpu` pin that batching and the
  glass seam are unchanged.

## 8. Polygons (slice 5, issue #673)

A `Polygon` record names a run of the frame's **vertex pool** with `first_vertex` and `vertex_count`;
its rect is the polygon's bounding box, so the scissor fast path works exactly as for the other kinds.
The pool is the buffer's last area: 4096 vertices of 8 bytes, 32 KB, which makes the buffer 53,264
bytes per frame in flight (`buffer_bytes(256, 256, 4096)`).

**Coverage.** One loop decides it, in `gpu::clip::inside_polygon` and in the shader's
`inside_polygon_record` (GLSL and WGSL), expression for expression. A ray runs from the pixel centre
toward +x; an edge counts when exactly one endpoint is at or above the centre's y (half-open, so a
vertex on the ray is counted once) and the edge crosses to the centre's right. `evenodd` is the parity
of the crossings and `nonzero` the signed sum of their directions. The anti-aliased path uses the
distance to the nearest edge for the magnitude and the same test for the sign.

**Limits.** At most `kMaxPolygonVertices` (256) per polygon and `kMaxClipVertices` (4096) per frame.
`ClipStateTable::push_shape` checks both and degrades a polygon over either to its bounding box, which
never paints outside the shape's own box, and counts it in `shape_fallbacks()` (also reachable as
`Batched2DRenderer::clip_shape_fallbacks()`). A polygon with fewer than three vertices has no area and
clips everything away, which is not a fallback. Identical polygons share their pooled run, so the same
shape pushed under several nodes costs its vertices once. Deeper than `kMaxClipDepth` a shaped clip
folds to its bounds, like any other.

**What is not wired yet.** The parser, the record, the renderer entry (`push_clip_shape`), the shaders
and the CPU reference exist. The paint pass does not read `clip-path` yet, so no stylesheet draws a
polygon until the rest of #615 lands (the paint wiring and hit testing).

**Tests.**
- `clip_polygon` pins the parser (fill rules, percentages, the box origin, every invalid form) and the
  coverage of a convex, a concave and a self-intersecting polygon under both rules.
- `clip_state_polygon` pins the pool, the deduplication, the two limits and the degenerate case.
- `polygon_clip` (real Vulkan) compares every pixel of six scenes with `CpuCanvas`: convex, concave,
  a star under both fill rules, a polygon under a rounded clip, and an over-limit polygon. Its fixtures
  keep every edge crossing at least a thousandth of a pixel from a pixel centre, so it fails only when
  the rule is wrong, not when two platforms round differently.
