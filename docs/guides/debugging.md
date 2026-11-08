# Debugging guide

Status: the tools and flags below exist as of Big Step B1 (core renderer); the diagnostics overlay and tracing are Big Step B2 work (see [AGENTS.md, Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step); design in [ADR-022](../../decisions/ADR-022-diagnostics-and-profiling.md), diagnostics and profiling). Sections marked (target) are not implemented.

## 1. First checks

- Build the `dev`/Debug preset with validation layers.
- Run the `null` backend (the RHI backend that records commands without a GPU) to isolate batching/command issues from the GPU.
- Run with lavapipe (Mesa's software Vulkan driver, used by CI) to reproduce CI/software behavior.
- Reproduce with a fixed size/DPI and the `--golden` flag (disables anti-aliasing for pixel-stable output).

```bash
./build/dev/tools/poc/batched2d/batched2d_poc --backend null
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/dev/tools/poc/batched2d/batched2d_poc --backend vulkan --golden --out test.png
```

## 2. Vulkan validation

- `batched2d_poc --validation` enables validation layers.
- Debug builds must run without validation errors; fix validation errors before visual bugs.
- Frequent causes: missing image-layout transitions, descriptor set mismatch, invalid scissor,
  destroyed resources still in use.
- Use `VK_LAYER_KHRONOS_validation` settings for synchronization validation when debugging
  races.

## 3. GPU tools

| Tool | Use |
|---|---|
| RenderDoc | frame capture, pipeline state, textures, shaders |
| NVIDIA Nsight Graphics | GPU timing, capture, shader debugging |
| PIX (Windows) | Direct3D 12 is not used by Tessera; PIX is only an OS-level capture option |
| Vulkan validation | API/synchronization errors |
| `VK_LOADER_DEBUG=all` | loader/ICD discovery issues |

Capture a deterministic frame (`--golden`, fixed size) so captures are comparable.

## 4. Demos and flags

`vulkan_offscreen_poc`:

```text
--out file.png --width N --height N --device N --validation|--no-validation
```

`batched2d_poc`:

```text
--backend vulkan|null --out file.png --raw file.rgba
--width N --height N --validation --golden
```

`--golden` disables AA for pixel-stable edges. `--raw` writes RGBA8 for byte-level comparison.

## 5. Logs and diagnostics (target)

- Structured logs with levels/sinks; never `printf`-only debugging.
- Trace spans around frame stages and GPU passes.
- Metrics: frame time per stage, draw calls, vertices, cache hit/miss, GPU memory.
- Overlay: FPS/frame time, dirty domains, batch counts, animation count.
- Crash dumps: bounded/redacted; symbols archived separately.

See [`diagnostics.md`](../design/diagnostics.md),
[`diagnostics-config-v1.md`](../specs/diagnostics-config-v1.md), and
[`devtools-protocol-v1.md`](../specs/devtools-protocol-v1.md).

## 6. Visual bugs

1. Reproduce with `--golden` and fixed size/DPI.
2. Compare draw calls/vertices with `--backend null`.
3. Check batching key/scissor/texture changes (see [`renderer.md`](../design/renderer.md)).
4. Check image layout/descriptor binding/validation.
5. Check color space/alpha/blend assumptions.
6. Add a minimal regression scene and, once the golden pipeline exists (Big Step B2), a golden test.

## 7. Layout/text bugs (target)

- Dump computed style/layout boxes (deterministic JSON).
- Check text measurement/shaping/fallback and font files.
- Check pixel rounding when the DPI scale is not 1.0.
- Reproduce with fixed locale/fonts/DPI.

## 8. Async/threading bugs

- Run TSan (thread sanitizer); look for UI-thread blocking calls.
- Check cancellation/shutdown paths and completion queue overflow.
- Ensure no async callback touches destroyed nodes/resources.
- Use deterministic executor/event scripts to reproduce.

## 9. Platform bugs

- Windows: check `WM_DPICHANGED` suggested rect, `WM_ENTERSIZEMOVE` coalescing, power GUIDs.
- X11: call `XRRUpdateConfiguration` after RandR events; check Present/sync hints.
- Wayland: never draw before `ack_configure` (the compositor's configure acknowledgement); the compositor owns size; check the `suspended` state.
- Android: configuration changes/fold may restart the app; check state restore.
- See [`os-events-v1.md`](../specs/os-events-v1.md) and
  [`window-resize.md`](../design/window-resize.md).

## 10. Common problems

| Symptom | Most probable cause |
|---|---|
| Black/empty image | target not cleared, wrong viewport/scissor, draw not submitted |
| Flicker/tearing | present mode/vsync, missing frame pacing |
| Stretched/offset UI | DPI scale or transform/rounding issue |
| Batch count too high | texture/scissor changes per quad; atlas/sort issue |
| Text missing | font fallback/shaping/atlas (GPU texture of glyphs) upload |
| Crash on resize | old swapchain/target destroyed while still in use by in-flight frames |
| Validation error | layout transition/descriptor/lifetime |
| CI fails but local passes | GPU driver vs lavapipe/deterministic mode |

## 11. Reporting a bug

Include:

- commit, OS, GPU/driver, backend/ICD
- exact command/flags, size/DPI/scale
- validation output
- minimal scene/project or event script
- screenshot/PNG/raw + logs/metrics
- whether `--backend null`/lavapipe reproduces it
