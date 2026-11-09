# Layering, scope and the Go decision

> [<- Measurements and Vulkan viability](03-measurements.md) | [Reproducing the spike ->](05-reproducing.md)

## 6. Layering and warning policy

- Only `src/rendering/path/thorvg_path_rasterizer.cpp` includes a ThorVG header. The
  configure-time gate in `src/rendering/path/CMakeLists.txt` scans every public header, `src/` and
  `tools/` source for `#include <thorvg...>`/`"thorvg..."` and fails configuration if any file
  other than that adapter matches; it runs in **every** configuration, including the default
  build where ThorVG is OFF.
- Public headers expose only seam types; UI code can only reach the rasterizer through
  `PathRasterizer`.
- Third-party sources compile with `-w`/`/W0` + C++17; Tessera sources (seam, spike tool) stay
  under `-Werror` and the project's C++23 baseline. The spike builds with
  `TESSERA_WARNINGS_AS_ERRORS=ON`.


## 7. Deliberately not done

- **No renderer backend integration.** No RHI texture upload, no `Path` scene node, no paint-pass
  wiring, no SVG asset loading path (the SVG loader compiles but is not exercised by the seam).
- **No Vulkan/WebGPU backend work** inside ThorVG, and no attempt to make `GlCanvas`/`WgCanvas`
  usable; the spike never creates a GPU surface.
- **No seam features beyond V1**: no gradients, dashes, masks, text-as-path, blend modes, or
  clipping. They are legal ThorVG features but not part of the seam yet.
- **No threading tuning**: worker threads are pinned to zero for determinism. A future
  deterministic tiling policy is a separate decision.
- **No performance characterization at scale**: the spike measures one 512x512 frame and a fixed
  icon/chart set, not animation, damage redraw, or large SVG scenes. Caching/tiling design comes
  with the real integration.


## 8. Recommendation: Go

Proceed with the full layout of the plan's P1 item 1: keep `TESSERA_ENABLE_THORVG` (default OFF
until the path feature ships), add the `Path` scene node and paint-pass integration, upload the
RGBA8 buffer through the RHI, add the SVG asset path, then revisit caching/tiling and MSAA (D5)
with real frames. No re-pin or alternative backend is needed for that work; the seam can stay
unchanged, which is the point of it.

The remaining decision for the Lead/owner is not Go/No-Go but **when** the option flips to ON by
default; this spike keeps it OFF so the default build and its artifacts stay unchanged.
