# Rendering plan to production: static linking and license-clean

Status: plan for the rendering workstream from the current B1/B2/B3 state to the production
milestone. The project owner approved **D1 (ThorVG first)** and **D2 (sRGB-encoded formats)** on
2026-11-09; D2 is implemented (RHI sRGB formats, swapchain preference policy, UNORM goldens
unchanged), D1's integration spike is next, and D3-D8 are engineering defaults enforced by the
release gates and remain open to owner review. The acceptance gates are normative for the release
build. Related:
[`renderer.md`](renderer.md), [`gpu/README.md`](gpu/README.md),
[`dependencies.md`](../specs/dependencies.md), [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md),
[ADR-012](../../decisions/ADR-012-static-linking.md), [ADR-013](../../decisions/ADR-013-project-license-mit.md),
[ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md).

## 1. Goal and constraints

The production build is a native application. It must ship **statically linked third-party
C/C++ code**, keep **system components** (Vulkan loader/ICDs, X11/Wayland, Win32, macOS
frameworks) dynamic, and contain **no copyleft obligation** (no LGPL/GPL/AGPL in linked code).
This is [ADR-012](../../decisions/ADR-012-static-linking.md) plus [ADR-013](../../decisions/ADR-013-project-license-mit.md) (MIT).

Two walls must stay separate:

| Wall | Contents | Shipped |
|---|---|---|
| Native production | Vulkan 1.3 backend, batched 2D, text shaping/atlas, paths (P1) | yes, static third-party |
| Design-time | Wasm engine + browser WebGPU via Emscripten; optional native Dawn for headless tools | no native binary dependency |

A dependency that only runs at build time (glslc, CMake, clang-format) or only in the design-time
tool (Dawn, Emscripten) must never appear in the production link line or the production notice
set.

## 2. Current state (what exists)

- RHI is header-only and 2D-first: buffers/textures/pipelines/targets/command lists, async
  submit, swapchain seam (`include/tessera/rendering/rhi/`).
- Vulkan 1.3 backend: dynamic rendering, volk + VMA, validation opt-in, async upload/submit,
  two-slot swapchain ring, Win32/X11 surfaces, offscreen readback.
- Batched 2D: solid/SDF-rounded/textured quads, clip/transform stacks, `begin_damaged` scissor
  replay, frames-in-flight slots (`include/tessera/rendering/gpu/`).
- Null backend and CPU canvas for deterministic tests; lavapipe/CPU pixel goldens; image diff.
- Box paint pass (style -> quads) wired into `FramePipeline::paint_frame` when a frame is due.
- Gaps relevant here: no vector paths, no depth/index/compute in the RHI, WebGPU backend empty,
  present-path damage/VRR absent. (sRGB policy and paint-time text landed: D2/D9.)

## 3. Decisions

D1 and D2 are **approved by the project owner (2026-11-09)**. D3-D8 are engineering defaults that
the release gates enforce; they can be revisited by the owner without changing the gates.

| ID | Decision | License/static impact |
|---|---|---|
| D1 | **Vector paths: ThorVG first (approved).** Spike ThorVG (MIT) behind a `PathRasterizer` seam; Skia Graphite stays a fallback only if the spike fails the gate. Do not write a rasterizer. | Both permissive and static-linkable; ThorVG is far smaller, so the production static size and build time stay bounded. |
| D2 | **Color: 8-bit sRGB-encoded formats in V1 (approved).** Prefer `*_SRGB` swapchain/target formats, document that blending happens in the encoded space (approximation), and keep linear-light/HDR out of V1. | No license impact; avoids a large color-management refactor before release. |
| D3 | **Design-time wall.** Dawn/WebGPU and Emscripten are design-time only; the native production binary links Vulkan + MoltenVK (macOS, later) and never Dawn. | Dawn/Emscripten notices apply to design-time artifacts only; production link line stays lean. |
| D4 | **Present-path damage and VRR are post-production optimizations.** Correct full-frame present plus the offscreen damaged path ship first. | No license impact; VRR depends on platform APIs that are still open questions. |
| D5 | **MSAA and depth/index/compute follow paths.** 4x MSAA is P1 for geometry quality; depth/index/compute (custom 3D) are P2 and widen the RHI only then. | Vulkan core features; no dependency change. |
| D6 | **Text AA stays coverage-based** (glyph atlas, 8-bit coverage); an SDF atlas is optional P1 for scaling, not a production blocker. | No license impact. |
| D7 | **Device loss is a production requirement**: explicit recoverable failure, no UI-thread indefinite waits; device-loss UX policy can stay minimal (recreate device once, surface error). | No license impact. |
| D8 | **Correctness gates**: validation-layer-clean debug runs, bit-exact software goldens, damage-redraw equivalence, resize/DPI tests. | No license impact. |
| D9 | **Text paint payload (Lead engineering default, owner-reviewable).** Scene nodes carry an optional `ui::PaintData` text payload (content + minimal font/color descriptor); visual properties keep coming from computed style. Rationale: no full text-node model in V1, layout and style stay authoritative, and the decision is reversible when rich text lands. | No license impact; keeps text drawing on the existing shaped-text/atlas path. |

Open questions that stay parked (not production blockers): VRR capability APIs, HDR/linear color,
custom-element 3D depth ordering, native Dawn feature floor, ICU data subset. Their features stay
out of the release until decided.

## 4. Workstreams

### P0 — production blockers

1. **Paint completeness — done**: damage-clipped paint via `RedrawPlan`; text runs in the paint
   pass via the D9 `ui::PaintData` payload (`PaintSink::draw_text` -> `TextRenderer`); rounded
   clip intersection with the scissor stack.
2. **Present correctness**: swapchain present/damage-free path on Win32/X11 already renders and
   presents; device-loss detection + one-shot recovery and runtime-loop orchestration landed
   (D7), bounded acquire/present policy tests, and swapchain image lifetime checks under
   validation.
3. **Color policy (D2) — done**: sRGB texture/render-target/swapchain formats in the RHI, the
   plain-data swapchain preference policy (sRGB surfaces first, UNORM fallback), documented
   encoded-space blending approximation; deterministic golden paths stay UNORM so existing CPU and
   lavapipe goldens are byte-identical.
4. **Static/license gates — done**: dependency/notice allowlist checker, per-OS static-link
   allowlist checker, forbidden dynamic C++ runtime rule, fixtures and CI wiring; a Release
   static-runtime proof build is in progress.
5. **Golden/validation corpus**: dashboard/overlay/text/shapes already; add damage-redraw,
   resize, and present-path structural tests.

### P1 — production quality

1. **ThorVG paths (D1)**: spike done — Go ([`thorvg-spike/README.md`](thorvg-spike/README.md)); pinned v1.1.3
   behind `TESSERA_ENABLE_THORVG` (default OFF), `PathRasterizer` seam and a null backend landed.
   Next: RHI texture upload, `Path` node, SVG asset path.
2. **MSAA 4x** for non-SDF geometry, resolve into the 2D target; golden updates deliberate.
3. **Present-path damage and frame pacing polish** (D4); VRR only if the platform APIs are decided.
4. **Text scaling**: SDF atlas or multi-size atlas (D6); line breaking/bidi once ICU/SheenBidi land.

### P2 — post-V1

Depth/index/compute RHI surface + custom 3D/compute elements, native WebGPU/Dawn parity tools,
HDR/linear color, multi-window/multi-display rendering.

## 5. Dependency, license and linkage matrix

Only rows marked *production-linked* need to pass the static and notice gates in the shipped
binary.

| Component | License | Linkage in production | Gate |
|---|---|---|---|
| volk | MIT | static (vendored) | notice |
| VMA | MIT | static (vendored) | notice |
| Yoga | MIT | static (vendored) | notice |
| FreeType | FTL (BSD-style, GPL option unused) | static (FetchContent) | notice + FTL text |
| HarfBuzz | MIT | static | notice |
| SheenBidi | Apache-2.0 | static (planned) | notice + Apache NOTICE |
| ICU | Unicode License v3 | static + data (planned) | notice + data license |
| ThorVG | MIT | static (P1) | notice |
| Skia Graphite | BSD-3-Clause | static, fallback only | notice |
| zlib / libpng / libjpeg-turbo / libwebp | zlib / libpng / BSD-3/IJG / BSD-3 | static | notices |
| Vulkan loader + ICDs | Apache-2.0 / vendor | **system dynamic** | not statically linked |
| X11 / Wayland / xkbcommon | MIT | **system dynamic** | allowlisted dynamic |
| Win32 / Cocoa / Android NDK | System | system | n/a |
| MoltenVK | Apache-2.0 | static (macOS, later) | notice |
| Dawn | BSD-3-Clause | design-time only | not in production link line |
| Emscripten + WebGPU bindings | MIT / UIUC | build-time | not in production link line |
| glslc / shaderc / SPIRV-Reflect | Apache-2.0 | build-time (SPIR-V is an asset) | not linked |
| ValidationLayers | Apache-2.0 | debug/CI only | not shipped |
| Mesa lavapipe | MIT | CI only | not shipped |
| ccache | GPL-3.0-or-later | separate build program | never linked |
| AT-SPI / libdbus | LGPL / AFL+GPL | **not linked** (D-Bus protocol later) | forbidden in link line |
| Fonts (Noto/Inter, test) | OFL-1.1 | test assets; shipping fonts needs OFL notices | notice when shipped |

Rules:

- Allowed licenses in linked code: MIT, BSD-2/3-Clause, Apache-2.0 (with NOTICE), FTL (not GPL
  option), Unicode, zlib, libpng, IJG, ISC, curl, BSL-1.0, OFL (assets only).
- Forbidden in linked code: GPL, LGPL, AGPL and any license with source-disclosure or
  relinking obligations on the shipped binary. System libraries accessed through OS facades are
  not linked third-party code; LGPL system integration (AT-SPI) stays runtime/protocol-only.
- Apache-2.0 components keep their `NOTICE` text; the project [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md)
  must list every linked component with files/upstream/copyright.
- No runtime shader compilation: SPIR-V is built by `glslc` at build time and shipped as data.

## 6. Release gates (definition of production-ready rendering)

1. **Static link**: a release build of a sample app has no unexpected dynamic third-party
   libraries. Allowlist is per OS: libc/libm/libdl/pthread (Linux system), X11/Wayland/xkbcommon
   and Vulkan loader (Linux system), KERNEL32/USER32/GDI32/SHELL32/ADVAPI32/ole32 and
   `vulkan-1.dll` (Windows system), `/usr/lib/libSystem`, `/System/Library/Frameworks/*`
   (macOS). `libstdc++`, `libgcc_s`, `VCRUNTIME*`, `MSVCP*` must be static/absent.
2. **License**: dependency-license allowlist scan passes; no forbidden license in the link
   line; every production-linked dependency has a notices entry; Apache NOTICE preserved.
3. **No design-time-only dependency** (Dawn, Emscripten) in the native production link line.
4. **No runtime shader compilation**; shaders are SPIR-V assets.
5. **Correctness**: validation layers clean on debug runs; bit-exact lavapipe/CPU goldens;
   damage-redraw equivalence; resize/DPI/lifecycle tests; bounded acquire/present (no indefinite
   UI-thread wait).
6. **Device loss**: recoverable path tested with a synthetic device-loss injection where the
   backend allows it; no crash or silent corruption.
7. **Docs in sync**: `docs/specs/dependencies.md`, `THIRD_PARTY_NOTICES.md`, this plan and the
   module map agree; status table in [`docs/README.md`](../README.md#8-status) reflects reality.

Gate 1-2 are automated by `scripts/check_static_link.*` and `scripts/check_dependencies.*`
(CI: Linux ldd, Windows dumpbin, macOS otool) plus the existing Android/iOS builds that use a
static C++ runtime. Gates 3-7 are checked by the CI test suite and review.

### Running the gates locally

The gates run as CI steps on every PR and can be run from the repository root with a plain
Python 3 interpreter. Both scripts are standard-library only, deterministic and use no network:

```bash
python scripts/check_dependencies.py
python scripts/check_static_link.py --binary build/ci/tools/poc/vulkan_offscreen/vulkan_offscreen_poc
python scripts/check_static_runtime.py --config Release
python -B -m unittest discover -s scripts/tests -p "test_*.py" -v
```

`check_static_link.py` selects `ldd`, `otool -L` or `dumpbin /dependents` for the current OS and
fails on any dynamic dependency outside the per-OS allowlist and on a dynamically linked
`libstdc++`, `libgcc_s`, `VCRUNTIME*` or `MSVCP*`. Pass `--os` (or `--from-output FILE`) to check
captured output for another platform and `--allowlist FILE` to replace the built-in pattern list.
`check_static_runtime.py` proves the requirement on a fresh Release artifact: it configures a
throwaway build with `TESSERA_STATIC_RUNTIME=ON` and `TESSERA_BUILD_VULKAN=OFF`, builds one small
target and runs the static-link gate on it (Debug is reported as `SKIPPED`, never passed).
The fixture self-tests feed both gates synthetic tables and `ldd`/`otool`/`dumpbin` output, so
they run on every OS; either gate accepts `--json` for a machine-readable report.

## 7. Risks

- **ThorVG Vulkan/WebGPU maturity** is unproven; the spike must be able to fail and fall back
  to Skia without changing the `PathRasterizer` seam.
- **Static size**: ThorVG/ICU are the largest additions; measure before committing and keep
  feature flags.
- **ICU data**: ship a custom subset, not the full data set.
- **VRR/HDR/3D** rely on decisions not yet made; they stay out of the production milestone.
- **Windows/macOS static checks** differ from Linux; the scripts need per-OS allowlists and a
  CI job per OS, otherwise a dynamic dependency can slip through on a platform that CI does not
  inspect.

## 8. Execution order

1. task-4 damage-clipped paint (in flight) -> text paint data decision.
2. task-8 static/license gates (parallel, independent).
3. Device-loss recovery and sRGB policy (P0) after the paint/present path is stable.
4. ThorVG spike (P1) once P0 gates are green; then paths, MSAA, present-path damage.
5. P2 items only after V1 scope is locked.
