# Measurements and Vulkan viability

> [<- Path model and rasterizer seam](02-path-model-and-rasterizer.md) | [Layering, scope and the Go decision ->](04-layering-scope-and-decision.md)

## 5. Measurements

Method: Windows 11, clang 22.1.5 (LLVM, GNU-like driver), CMake 4.1.1, Ninja, `-j12`, Vulkan
backend OFF (`TESSERA_BUILD_VULKAN=OFF`), `TESSERA_WARNINGS_AS_ERRORS=ON`. Timed builds run with
the compiler cache disabled so the numbers are cold. Configure timings include the FetchContent
downloads (network), which is why they are much larger than the compile time.

| Metric | Release | Debug | Notes |
|---|---|---|---|
| Clean configure, option ON | 72.0 s | 49.6 s | includes fetching ThorVG (3.6 MiB), FreeType and HarfBuzz |
| Clean build of `thorvg_spike` | 12.2 s | 12.6 s | 50 Ninja steps, 37 ThorVG translation units, `-j12` |
| `tessera_thorvg` static archive | 915,564 B (894 KiB) | 4,099,462 B (3.91 MiB) | Debug size is debug info |
| `thorvg_spike.exe` | 569,344 B (556 KiB) | 1,381,888 B (1.32 MiB) | links seam + golden + stb |
| Null-only reference exe (same sources, no ThorVG) | 225,792 B | - | delta attributable to ThorVG: **343,552 B (335.5 KiB)** |
| Startup (process entry to rasterizer ready) | 0.02-0.04 ms | 0.06-0.49 ms | `--backend thorvg` |
| First render, 512x512, 15 shapes | 1.0-1.7 ms | 4.0-4.8 ms | `rasterize()` incl. `draw()`/`sync()` |
| Process total incl. PNG write | 13-26 ms | 111-122 ms | Debug PNG encoding dominates at `-O0` |
| Peak working set, 512x512 | 6.53 MiB | - | includes pixel buffer, ThorVG, and PNG encode |

Interpretation:

- **Build cost is acceptable.** 12 s on top of a clean build, ~894 KiB Release archive; the final
  binary grows by ~336 KiB because the linker drops unused ThorVG code. That is far below the
  size risk the plan flagged for ICU/Skia.
- **Startup is negligible**; first-render cost is CPU rasterization of the path set, not
  initialization. A single static 512x512 icon frame is ~1 ms in Release, so a UI with a handful
  of icon-sized paths is not CPU-bound at 60 Hz. Large animated path sets will need caching and
  damage-scissored re-rasterization (follow-up, not a spike requirement).
- **Memory is small**: no per-frame allocation is visible beyond ThorVG's raster pool; the peak
  includes the 1 MiB RGBA buffer plus PNG encoding scratch.

### Vulkan viability

**There is no ThorVG Vulkan engine.** v1.1.3 exposes `SwCanvas` (CPU), `GlCanvas` (OpenGL/EGL) and
`WgCanvas` (WebGPU); the compiled `gpu_engine/` directory holds only the GL and WebGPU backends.
RHI integration therefore is, and must be, **CPU rasterize into an RGBA8 buffer, then upload the
buffer through the Tessera RHI** (a texture upload, not a new renderer backend). On the design-time
WebGPU side, ThorVG's `WgCanvas` is not a substitute for Dawn and is out of scope here; the seam
keeps that option open without committing to it.
