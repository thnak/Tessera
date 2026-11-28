# Glyph rasterization: what the CPU costs today, and where a GPU route could pay

- Status: measured (2026-11-27), recommendation only. Issue #686. Step 2 (a path-renderer or SDF prototype)
  is not started: it needs the production path renderer of #685.
- Related: [`display-aware-rendering.md`](display-aware-rendering.md) section 3 (hinting, LCD),
  [`gpu/compute.md`](gpu/compute.md), [ADR-071](../../decisions/ADR-071-gpu-compute-tile-rasterizer.md).

Glyphs are rasterized by FreeType on the CPU into a bounded atlas (`src/ui/text/glyph_atlas.cpp`,
`glyph_raster.cpp`). The question of #686 is whether that should move to the GPU. This note records what
it costs, measured by `tests/unit/glyph_raster_measure.cpp` (not a test; run it by hand in Release with a
font path). Machine: Ryzen 5 9600X, one thread, NotoSans-Regular, the printable ASCII range.

## 1. Cost of one cache miss

| Pixel size | Grayscale us/glyph | Grayscale bytes | LCD us/glyph | LCD bytes |
|---|---|---|---|---|
| 12 | 26.8 | 52 | 27.7 | 171 |
| 16 | 27.8 | 87 | 28.8 | 282 |
| 24 | 38.2 | 177 | 29.6 | 566 |
| 32 | 28.0 | 325 | 31.8 | 1 023 |
| 64 | 29.3 | 1 220 | 44.4 | 3 758 |
| 128 | 31.3 | 4 778 | 43.3 | 14 523 |
| 256 | 30.9 | 18 979 | 81.6 | 57 282 |
| 512 | 73.5 | 75 467 | 174.1 | 227 163 |

The cost is **flat at about 28 us from 12 to 256 px**: it is per-glyph overhead (load, hint, render, copy
into a fresh vector), not area. Area starts to matter at 512 px grayscale and at 256 px in LCD, where the
bitmap is three times larger. So the "large display text pays area-proportional cost" worry in the issue is
true only above 256 px. (The 24 px grayscale row is one noisy median; the neighbours say the curve is flat.)

## 2. Rebuild when the render scale steps

A UI that draws 4 text sizes from 95 ASCII glyphs rasterizes 380 glyphs after a scale step (1.0 to 1.25, 1.5,
2.0, 3.0): **11 to 13 ms**, on the UI thread today, which is about one frame at 90 Hz and well over one at
144 Hz. It is the per-glyph overhead again (29 us x 380), so it grows with the number of distinct glyphs and
sizes on screen, not with the scale. A CJK or mixed-script screen has far more than 380.

The upload is 4 MiB for one 1024x1024 RGBA8 page. `take_dirty` reports the whole page for a new page, so the
dirty-rectangle saving of #681 does not apply to a rebuild: the real coverage is about 380 x 350 bytes,
roughly 130 KiB, and the other 97% of the upload is empty texels. That is a separate, cheap fix (upload the
used shelf extent of a new page, not its full extent).

**Fixed by #949 (2026-11-28).** The page texture is created without pixels and its first write is the used
extent: full-width rows down to the last shelf plus one padding row, which is also what a linear filter can
read past a glyph. A later glyph that reaches rows the texture never got widens its rectangle to those rows,
so every row above the used extent is in sync; the #681 rectangle path is otherwise unchanged. Measured with
`glyph_raster_measure` (the "dirty-rect MiB" column, 1024x1024 page, 4 MiB before):

| Scale | 1.00 | 1.25 | 1.50 | 2.00 | 3.00 |
|---|---|---|---|---|---|
| Uploaded MiB | 0.68 | 0.92 | 1.21 | 1.46 | 3.05 |

It scales with the shelves in use, so the saving is 83% at scale 1 and 24% at 3.0 where the page is nearly
full. The rows are full width (a shelf's unused right side is uploaded), which keeps the invariant simple.

## 3. Scrolling a text column

A 400-line, 80-column column, 40 lines visible, one line per frame: the cold frame misses 95 glyphs (2.7 ms)
and uploads one 512x512 page (1 MiB); the **next 359 frames upload 0 bytes** and spend 0.063 ms looking up
3200 glyphs. Scrolling is not a rasterization cost at all once the working set is resident. Only new glyphs
(a new script, a new size) cost anything.

## 4. What this says about a GPU route

- **Small text (under 64 px).** The CPU cost is per-glyph call overhead, about 28 us. A GPU outline pass
  pays a dispatch plus a readback-free copy per batch, so it only wins when thousands of glyphs go in one
  batch, which is the rebuild case, not the steady state (0 us, section 3). Keep FreeType here: it is the
  reference for goldens, carries the hinting and LCD rules of display-aware-rendering section 3, and the GPU
  route would have to reproduce them bit for bit to keep those goldens.
- **Large text (256 px and up).** Area-proportional cost is real (74 us grayscale, 174 us LCD at 512 px, with
  75 to 227 KB per glyph). This is where outlines through the compute path renderer, or an SDF/MSDF atlas,
  could pay, because one SDF bitmap serves every size. It also removes the per-size atlas entries that make a
  scale step rebuild everything. This is a **hypothesis**, not measured: the path renderer of #685 is not in
  production and no SDF comparison exists.
- **The scale-step stall (section 2) has a cheaper fix than a GPU rasterizer:** rasterize the new sizes on a
  worker (the async primitives of `src/runtime/async.cpp` exist) and keep drawing the old-scale glyphs until
  the new page is ready, then swap. That removes the 11 to 13 ms from the UI thread without changing a pixel
  of output.

## 5. Recommendation

1. Keep FreeType on the CPU as the reference and for sizes up to 256 px.
2. Move the scale-step rebuild off the UI thread (worker rasterization, old page until the swap) and upload
   the used extent of a new atlas page instead of the whole page. Both are CPU-side, byte-identical to today,
   and open as follow-ups.
3. A GPU outline route for batch rebuilds of display-size glyphs is worth building as an option (section 6);
   it is not a replacement for FreeType on small text.

## 6. GPU outlines measured (#686 step 2, 2026-11-27)

`glyph_gpu_raster_measure <font.ttf>` decomposes the 94 printable ASCII outlines of Noto Sans into one
scale-independent document each and rasterizes them through `GpuPathRasterizer` (#685) into S x S
textures. RTX 4060 desktop, Release, median per glyph. "Batch" is all 94 in one flush (an atlas rebuild
on a scale step); "single" is one glyph per flush (one new glyph).

| Pixel size | encode (once, any size) | batch CPU | batch GPU | single CPU | single GPU wall | FreeType gray miss (section 1) |
|---|---|---|---|---|---|---|
| 128 | 5.6 us | 1.0 us | 3.8 us | 10.9 us | 75.6 us | 31 us |
| 256 | 4.5 us | 1.3 us | 10.4 us | 10.9 us | 97.5 us | 31 us |
| 512 | 5.4 us | 1.3 us | 35.8 us | 10.9 us | 138.8 us | 74 us |

What it says:

- **Rebuilds.** The UI-thread cost of a rebuild falls from about 29 us per glyph (11 to 13 ms for 380
  glyphs) to about 1 us per glyph, and the glyphs are on the GPU already: no bitmap, no upload. The GPU
  time per glyph is small at 128 px (3.8 us) and grows with area (35.8 us at 512 px), so one 94-glyph
  set at 512 px is 3.4 ms of GPU time.
- **Single glyphs.** The wall time to complete is 76 to 139 us (submit round trip), but the UI thread
  pays 11 us and does not wait; the glyph shows one frame later, like any async raster.
- **The encode is a one-time, size-independent cost** (about 5 us per glyph): the same document serves
  every size, so the per-size atlas entries stop being a rebuild cost.
- **What this does not show.** The output is straight-alpha RGBA8, not an R8 coverage atlas, so a real
  route needs to rasterize into atlas regions (or copy), and a coverage-only output pass; outlines are
  unhinted and carry no LCD subpixel rendering, so text under about 64 px (where `display-aware-rendering`
  section 3 hinting matters, and where FreeType goldens are the reference) stays on FreeType. Only an
  RTX 4060 was measured; the weaker GPUs of ADR-071 are slower at large areas. An SDF/MSDF atlas was
  not prototyped.

Recommendation after step 2: keep FreeType for goldens and small sizes; build the GPU outline route as
an opt-in for batch rebuilds at 128 px and above, after the worker-thread rebuild and used-extent
upload fixes of section 5 (cheaper, no pixel change) and measured against them.
