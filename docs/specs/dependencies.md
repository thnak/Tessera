# Dependency map (v0.1)

Tessera buys infrastructure and builds differentiators. We use mature libraries for layout,
text, vector paths, and GPU plumbing; we build the scene graph, compiler, runtime model, and
the headless agent loop ourselves.

This file is the canonical list. Every new dependency must be added here with its license,
module, role, and status.

## Principles

1. **Buy before build** for solved infrastructure (layout, text shaping, vector paths).
2. **Permissive licenses only** in the shipped core: MIT / BSD / Apache-2.0 / BSL-1.0 / zlib /
   OFL / Unicode / FreeType / public domain. No GPL/LGPL in the core.
3. **Build-time tools are not runtime dependencies.** The JSX parser, formatter, and shader
   compiler must not be linked into production apps.
4. **Pin everything.** Versions, hashes, fonts, lavapipe, shader compiler. Produce an SBOM (software bill of materials).
5. **Determinism first.** No system fonts, no wall clock, no floating driver versions in
   golden tests ([ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md), deterministic headless golden tests).
6. **Watch the big four.** Skia (or ThorVG), ICU (International Components for Unicode), Dawn, and Emscripten dominate build time
   and binary size; make them optional where possible.

## Status legend

- **V1 hard** — required for V1 (V1 is the first-release scope, [AGENTS.md](../../AGENTS.md) Non-negotiables rule 8)
- **V1 optional** — feature-gated, not needed for the first vertical slice
- **Build-time** — not shipped in the runtime
- **Candidate** — decision pending a spike
- **Deferred** — after V1 (no release assigned)
- **Future** — long-term, not committed

Acronyms and project terms used below (RHI, SPIR-V, lavapipe, VRR, damage, golden, IR, HAL) are defined in the [glossary](../reference/glossary.md).

## 1. GPU and rendering

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| Vulkan SDK (Headers, Loader, `glslc`, ValidationLayers) | V1 hard | Apache-2.0 | `rendering/backend/vulkan` | Vulkan API, loader, GLSL→SPIR-V, validation |
| VMA (VulkanMemoryAllocator) | V1 hard (vendored) | MIT | `rendering/backend/vulkan` | GPU memory allocation; `third_party/vma` |
| volk | V1 hard (vendored) | MIT | `rendering/backend/vulkan` | Vulkan function loading; `third_party/volk` |
| SPIRV-Reflect | V1 optional | Apache-2.0 | `rendering/backend/vulkan` | Shader reflection for pipeline layouts |
| stb_image_write | PoC (proof of concept)/demo only | Public domain / MIT | `tools/poc` | PNG output in the Phase 0 (proof-of-concept stage, executables under `tools/poc/`) and batched2d demos |
| stb_image | V1 test | Public domain / MIT | `tests/golden`, image decode | PNG/JPG decode for goldens and assets |
| ThorVG | V1 target (owner-approved 2026-11-09; integration spike pending) | MIT | `rendering/backend` | Vector paths, SVG |
| Skia Graphite | Fallback only ([ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md); owner chose ThorVG first) | BSD-3-Clause | `rendering/backend` | Vector paths/text, Vulkan backend |
| Dawn | V1 optional (native/headless) | BSD-3-Clause | `rendering/backend/webgpu` | Native WebGPU implementation; the Wasm viewer uses the browser WebGPU via Emscripten |
| Emscripten | V1 hard for design-time | MIT / UIUC | `designer/web` | C++ → WebAssembly toolchain |
| Emscripten WebGPU bindings (`webgpu.h` / emdawnwebgpu) | V1 hard for design-time | MIT / UIUC | `designer/web` | Browser WebGPU API |
| Mesa lavapipe | V1 CI | MIT | tests / CI | Software Vulkan ICD (installable client driver, the plug-in the Vulkan loader uses to reach a GPU or software rasterizer) for deterministic goldens |
| Vulkan ValidationLayers | V1 debug | Apache-2.0 | dev / CI | Validation |
| naga | Future ([ADR-011](../../decisions/ADR-011-shader-authoring-and-cross-compilation.md), shader authoring and cross-compilation) | MIT / Apache-2.0 | `tools/shaders` | SPIR-V → WGSL |
| Tint | Future ([ADR-011](../../decisions/ADR-011-shader-authoring-and-cross-compilation.md)) | BSD-3-Clause | `tools/shaders` | WGSL → SPIR-V |
| Slang | Future ([ADR-011](../../decisions/ADR-011-shader-authoring-and-cross-compilation.md)) | Apache-2.0 (LLVM exception) | `tools/shaders` | Single-source shaders |
| SPIRV-Cross | Future | Apache-2.0 | `tools/shaders` | SPIR-V → HLSL/GLSL/MSL if needed |
| zlib | V1 transitive | zlib | `rendering/resources` | PNG/texture compression |

VMA and volk are vendored from Vulkan SDK 1.4.357.0 (`Include/vma`, `Include/Volk`) into
`third_party/vma` and `third_party/volk`, so Linux CI does not depend on optional SDK
components. Their MIT headers are preserved.

Frame pacing, refresh-rate/VRR (variable refresh rate) queries, and damage (screen region to repaint) use system APIs (DXGI, Vulkan present modes,
X11 RandR/Present, Wayland `wp_presentation`). They are system components, not linked
dependencies. See [`runtime-config-v1.md`](runtime-config-v1.md) and
[`game-loop.md`](../design/game-loop.md).

## 2. Layout

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| Yoga | V1 hard (vendored v3.2.2) | MIT | `ui/layout` | Flexbox engine; `third_party/yoga` |
| taffy | Candidate | MIT | `ui/layout` | Grid engine via C ABI if we do not build a subset |
| — | — | — | `ui/style` | CSS parser/cascade is custom; no library |

## 3. Text

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| FreeType | V1 hard (FetchContent 2.13.3) | FreeType License (BSD-style) | `ui/text` | Font loading and glyph rasterization; fetched from a pinned archive hash with image codecs disabled |
| HarfBuzz | V1 hard (FetchContent 10.2.0) | MIT | `ui/text` | Shaping: ligatures, kerning, complex scripts; fetched from a pinned archive hash without the FreeType/ICU/GLib integrations |
| SheenBidi | V1 hard | Apache-2.0 | `ui/text` | Bidirectional text |
| ICU | V1 hard | Unicode License | `ui/text`, `services/i18n` | Segmentation, normalization, locale, optional collation |
| libunibreak | V1 optional | zlib | `ui/text` | Line breaking if ICU is too heavy |
| Noto Sans / Inter (test fonts) | V1 test | OFL-1.1 | `tests/fixtures/fonts` | Deterministic golden text |

FreeType is fetched at configure time from the pinned 2.13.3 archive; `FT_DISABLE_ZLIB`,
`FT_DISABLE_BZIP2`, `FT_DISABLE_PNG`, `FT_DISABLE_HARFBUZZ`, and `FT_DISABLE_BROTLI` keep the
build self-contained and deterministic. The archive hash pins the exact source, and no FreeType
type appears in a public header.

HarfBuzz is fetched the same way from the pinned 10.2.0 archive with its utils, tests, subsetter,
FreeType/ICU/GLib/Graphite2 and platform shaper integrations disabled. Tessera hands HarfBuzz the
font bytes that FreeType already validated, so the two libraries never share a face object.

## 4. Images and assets

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| libpng | V1 optional | libpng | `ui/image` | PNG decode |
| libjpeg-turbo | V1 optional | BSD-3-Clause / IJG | `ui/image` | JPEG decode |
| libwebp | V1 optional | BSD-3-Clause | `ui/image` | WebP decode |
| lunasvg | V1 optional | MIT | `ui/image` | SVG rasterization if ThorVG is not adopted |
| miniaudio | V1 optional | Public domain / MIT-0 | `hal/audio` | Audio hooks, notification sounds |

## 5. Compiler and codegen (build-time)

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| tree-sitter / SWC / oxc | Rejected ([ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md), own JSX/TSX front end and React-like model) | - | - | Replaced by Tessera's own JSX/TSX front end; tree-sitter may be reconsidered for editor highlighting only |
| clang-format | V1 hard (toolchain) | Apache-2.0 (LLVM exception) | `compiler/codegen`, `scripts/` | Deterministic formatting before merge ([ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md) (protected regions and binding manifest), [ADR-015](../../decisions/ADR-015-code-style-build-cache.md) (code style and build cache)) |
| clang-tidy | V1 build-time | Apache-2.0 (LLVM exception) | `scripts/`, CI | Static analysis ([ADR-015](../../decisions/ADR-015-code-style-build-cache.md)) |
| sccache | V1 optional build | Apache-2.0 | build | Compiler cache (preferred) |
| ccache | V1 optional build | GPL-3.0-or-later | build | Compiler cache; build-time only, never linked/shipped |
| EditorConfig | V1 tool config | — | repo | Whitespace and line-ending consistency |
| nlohmann/json | V1 build-time | MIT | `compiler/ir`, manifest | IR/manifest JSON |
| simdjson | V1 optional | Apache-2.0 | `compiler` | Faster JSON if needed |
| Inja | V1 optional | MIT | `compiler/codegen` | Template engine; prefer plain string builders |
| CMake | V1 build | BSD-3-Clause | build | Build system |
| vcpkg | V1 build (recommended) | MIT | build | Dependency management |
| Ninja | V1 optional | Apache-2.0 | build | Fast builds |
| Node.js | Not required ([ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md)) | MIT | - | Was the host for SWC/oxc; not a build dependency |
| Python 3 | Build-time | PSF | `scripts` | Tooling and CI helpers |

## 6. Platform and HAL

| Library / API | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| Win32 API | V1 (Windows) | System | `platform/windows` | Window, input, lifecycle |
| X11 (`libX11`) | V1 (Linux, B3 window/present; linked dynamically) | MIT/X11 | `platform/linux`, `rendering/backend/vulkan` | Window, Xlib `VkSurfaceKHR` |
| Wayland (`libwayland-client`, `wayland-protocols`) | V1 (Linux) | MIT | `platform/linux` | Window, input |
| libxkbcommon | V1 (Linux) | MIT | `hal/input` | Keyboard mapping |
| AndroidX WindowManager (`androidx.window:window`) | Android V1 optional | Apache-2.0 | `hal/window`, `ui/layout` | `WindowMetrics`, fold posture/hinge via JNI (Java Native Interface) |
| Cocoa / AppKit | Deferred (macOS, [ADR-010](../../decisions/ADR-010-macos-via-moltenvk-deferred.md) (macOS via MoltenVK, deferred)) | System | `platform/macos` | Window, input |
| GLFW | Candidate (buy) | zlib | `hal/window` | Window/input abstraction |
| SDL3 | Candidate (buy) | zlib | `hal/window`, `hal/audio` | Window/input/audio abstraction |
| inotify / ReadDirectoryChangesW / FSEvents | V1 | System | `hal/filesystem` | File watching |
| Vulkan surface extensions | V1 | Apache-2.0 | `rendering/backend/vulkan` | `KHR_surface` + platform surfaces |
| UIA (UI Automation) / AT-SPI (Assistive Technology Service Provider Interface) / NSAccessibility | Deferred | System (AT-SPI: LGPL) | `ui/accessibility` | Accessibility |
| D-Bus / libdbus | Deferred | System | `ui/accessibility` | AT-SPI transport on Linux |

Mobile/foldable signals use AndroidX WindowManager via JNI; iOS/iPadOS/macOS use system scene
APIs. No platform type leaks into `ui/`. See [`os-events-v1.md`](os-events-v1.md).

## 7. Services

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| libcurl | V1 optional | curl (MIT-like) | `services/networking` | HTTP/HTTPS client |
| IXWebSocket | V1 optional | BSD-3-Clause | `services/networking` | WebSocket client |
| mbedTLS | V1 optional | Apache-2.0 | `services/networking` | TLS |
| OpenSSL | V1 optional alternative | Apache-2.0 | `services/networking` | TLS alternative |
| libsodium | V1 optional | ISC | `services/networking` | Crypto primitives |
| sqlite3 | V1 optional | Public domain | `services/storage` | Local structured storage |
| zstd | V1 optional | BSD-3-Clause | `services/storage` | Compression |

## 8. Diagnostics

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| spdlog | V1 optional | MIT | `diagnostics/logging` | Async structured logging |
| Tracy | V1 optional | BSD-3-Clause | `diagnostics/profiling` | CPU/GPU profiling |
| Perfetto SDK | V1 optional | Apache-2.0 | `diagnostics/tracing` | System trace/Perfetto export |
| Crashpad | V1 optional | Apache-2.0 | `diagnostics/crash` | Crash dumps |
| Breakpad | V1 optional alternative | BSD-3-Clause | `diagnostics/crash` | Crash dumps alternative |
| {fmt} | V1 optional | MIT | `core`, `diagnostics` | Formatting where `std::format` is insufficient |
| ETW / ftrace / systrace | System | OS | `diagnostics/tracing` | Platform tracing backends (no link dependency) |

The async runtime uses C++20 coroutines and a small in-house executor/cancellation layer; it has
no linked dependency. Diagnostics backends are optional build flags and stay behind interfaces
(see [`diagnostics.md`](../design/diagnostics.md)).

## 9. Testing and CI

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| Catch2 | V1 test | BSL-1.0 | `tests/unit` | Unit tests |
| doctest | Candidate alternative | MIT | `tests/unit` | Lighter unit tests |
| GoogleTest | Candidate alternative | BSD-3-Clause | `tests/unit` | Unit tests |
| Google Benchmark | V1 optional | Apache-2.0 | `tests/benchmarks` | Microbenchmarks |
| stb_image | V1 test | Public domain / MIT | `tests/golden` | PNG decode and diff |
| Mesa lavapipe | V1 CI | MIT | tests / CI | Software Vulkan |
| libFuzzer | V1 optional | Apache-2.0 (LLVM) | `tests/fuzz` | Fuzzing |
| GitHub Actions | V1 CI | — | `.github/` | CI |

## 10. Foundational

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| C++23 standard library | V1 hard | — | all | `pmr`, threads, filesystem, `format`, `expected` |
| glm | V1 optional | MIT | rendering / ui | Math library; we may keep a small 2D math header instead |
| mimalloc | V1 optional | MIT | `core/memory` | General allocator |
| rpmalloc | V1 optional alternative | MIT / Public domain | `core/memory` | Allocator alternative |

## Not dependencies (deliberate)

- **Qt, Slint, Flutter, Electron, Chromium/WebView** — the framework owns its renderer.
- **A React runtime in production** — production is pure C++ ([AGENTS.md](../../AGENTS.md)).
- **Platform text APIs for rendering** (DirectWrite, CoreText) — not cross-platform or
  deterministic ([ADR-006](../../decisions/ADR-006-text-stack.md), text stack).
- **A Vulkan-on-WebGPU shim** — design-time uses Dawn directly ([ADR-002](../../decisions/ADR-002-webgpu-dawn-for-wasm.md), WebGPU/Dawn for Wasm).
- **A general 3D engine** — the renderer is 2D-first ([ADR-003](../../decisions/ADR-003-thin-rhi-and-batched-2d-renderer.md), thin RHI and batched 2D renderer).
- **A browser layout engine** — Yoga and a small Grid subset cover V1 ([ADR-005](../../decisions/ADR-005-yoga-layout-adapter.md), Yoga layout adapter).
- **GPL/LGPL libraries in the shipped core** — system-only access is allowed for the accessibility integration marked Deferred
  in section 6.

## Dependency budget and risks

- **Big four by cost:** Skia or ThorVG, ICU, Dawn, Emscripten. Measure build time and binary
  size before committing; keep them feature-gated.
- **ICU data** can be reduced with a custom data build; do not ship the full data set blindly.
- **Dawn** is large; it is only needed for the design-time viewer, not the native runtime.
- **Skia** is large; if chosen, use a minimal build (no codecs we do not need).
- **Supply chain:** pin versions and hashes, generate an SBOM, run a license scan in CI.
- **Determinism:** pin lavapipe, shader compiler, font files, and Unicode/ICU data versions.

## Open dependency decisions

1. **Windowing:** native HAL (Win32/X11/Wayland/Cocoa) vs GLFW/SDL3 behind `hal/window`.
   Recommendation: SDL3 or GLFW for V1, so platform work does not block the renderer;
   replace with native backends if needed (no release or trigger condition is specified).
2. **JSX parser:** resolved in [ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md) (own front end).
3. **Vector paths:** resolved by the project owner (2026-11-09): ThorVG is the V1 target (MIT,
   static), Skia Graphite is a fallback only if the integration spike fails
   ([ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md)). The spike measures build
   size, startup, memory, determinism and Vulkan/WebGPU viability before the dependency is pinned.
4. **Shaders:** GLSL + WGSL in parallel vs a single-source path via Slang/naga/Tint ([ADR-011](../../decisions/ADR-011-shader-authoring-and-cross-compilation.md)).
5. **Image codecs:** stb only (simple) vs libpng/libjpeg-turbo/libwebp (production).
6. **Networking:** libcurl + IXWebSocket + mbedTLS vs a smaller std-only stack.

## Integration strategy

- **vcpkg manifest** for most C/C++ dependencies; a lockfile and CI restore.
- **FetchContent** for fast-moving or non-portable dependencies (Dawn, ThorVG, Yoga) with
  pinned tags and shallow clones.
- **Vendored single-header** only for stable, tiny libraries (stb, VMA, volk) — with the
  license recorded in `third_party/README.md`.
- **Feature flags** for optional stacks (ICU, Skia, audio, networking, diagnostics) so a
  minimal native build stays small.

## Static linking (ADR-012 — hard requirement)

Decision record: [ADR-012](../../decisions/ADR-012-static-linking.md) (static linking). Tessera and every third-party C/C++ library are linked **statically** into the application.
The goal is a single self-contained executable with no third-party DLLs/`.so` shipped
alongside it.

### Scope

Statically linked:

- Tessera libraries
- all permissive third-party C/C++ libraries (Yoga, FreeType, HarfBuzz, SheenBidi, ICU,
  ThorVG/Skia, zlib/libpng/libjpeg-turbo/libwebp, VMA, volk, SPIRV-Reflect, spdlog, Tracy,
  libcurl, mbedTLS, libsodium, sqlite3, zstd, `{fmt}`, glm, ...)
- the C/C++ runtime where supported (`/MT`, `-static-libstdc++ -static-libgcc`)

Still dynamic, by platform necessity:

- the Vulkan loader (`vulkan-1.dll` / `libvulkan.so.1`) and GPU ICDs/drivers
- OS frameworks: Win32/User32/GDI, X11/Wayland, Cocoa/AppKit, Android system libraries
- glibc on Linux (fully static glibc is not required; use musl if a fully static Linux
  binary is ever required)

### License consequences

- **No LGPL/GPL in statically linked code.** LGPL static linking requires relinkable object
  files and reverse-engineering permission; GPL static linking relicenses the whole app.
- **AT-SPI/libatspi/glib (LGPL-2.1) must not be linked.** Linux accessibility goes through the
  D-Bus protocol or an optional runtime-only integration.
- **libdbus (AFL-2.1/GPL)** must not be linked; use the D-Bus protocol/system service.
- Notices must be embedded or shipped: `THIRD_PARTY_NOTICES.md` and an About/licenses UI.
- Apache-2.0 `NOTICE`, MIT/BSD copyright, OFL font licenses, FreeType FTL, Unicode/ICU notices
  must be preserved.

### Build and verification

- CMake option `TESSERA_STATIC_RUNTIME` (default ON): `/MT` on MSVC,
  `-static-libstdc++ -static-libgcc` on GNU/Linux.
- `BUILD_SHARED_LIBS=OFF`.
- ICU: static data packaging (`--with-data-packaging=static`) or embedded data via
  `udata_setCommonData`.
- CI checks the link graph: `dumpbin /dependents` (Windows), `ldd`/`readelf -d` (Linux),
  `otool -L` (macOS). Unexpected third-party dynamic libraries fail the build.
- Phase 0 evidence (Windows/MSVC, `/MT`): `vulkan_offscreen_poc.exe` imports only
  `vulkan-1.dll` and `KERNEL32.dll`; `merger_poc.exe` imports only `KERNEL32.dll`.
- The Vulkan loader may be statically linked from source (Apache-2.0), but it still loads GPU
  ICDs dynamically; a zero-DLL executable is not achievable with Vulkan on Windows.

## Component map and data flow

This is the concrete answer to "which library does what, in which component".

### Build-time: JSX to C++

```text
.jsx/.tsx
  -> [own JSX/TSX front end]           parse to AST              compiler/analyzer
  -> [Tier 1 analyzer]                 React rules, purity       compiler/analyzer
  -> [Tier 2 rules + bind extractor]   lower to IR                compiler/rules
  -> [nlohmann/json]                   IR/manifest JSON           compiler/ir
  -> [codegen]                         emit C++                   compiler/codegen
  -> [clang-format]                    deterministic format       build step
  -> [merger]                          preserve dev regions       compiler/merger
  -> .cpp/.h
```

- **Tier 1 / Tier 2** — Tier 1 is the analyzer for React rules and purity; Tier 2 lowers to the IR (intermediate representation) and extracts `bind::` bindings ([`compiler.md`](../design/compiler.md) sections 4 and 5).
- **Own front end** — Tessera lexer/parser for the JSX/TSX subset ([ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md)). Produces an AST; no UI semantics.
- **nlohmann/json** — serializes the IR and the binding manifest; also used for snapshot
  tests. Not on the runtime hot path.
- **clang-format** — invoked as a build tool, never linked into the app.
- **Codegen and merger** — our own code; no external library.

### Runtime: one frame

```text
input event
  -> scene graph update (dirty flags)              ui/view
  -> layout                                        ui/layout (Yoga)
  -> text pipeline                                 ui/text
       ICU segmentation -> SheenBidi bidi -> HarfBuzz shaping -> FreeType raster -> atlas
  -> style resolution                              ui/style (custom CSS subset)
  -> path fill/stroke                              ui/view -> rendering (ThorVG/Skia)
  -> 2D batching (quads, SDF, glyphs, clips)       rendering (our code)
  -> RHI                                           rendering/rhi
  -> Vulkan                                        rendering/backend/vulkan
       volk (function loading) + VMA (memory) + SPIRV-Reflect (binding metadata)
  -> GPU
```

- **2D batching** (our code) groups quads, SDF (signed distance field) rounded rects, glyphs, and clips into few draw calls ([ADR-003](../../decisions/ADR-003-thin-rhi-and-batched-2d-renderer.md)).
- **Yoga** computes boxes; it does not paint.
- **ICU** segments text (grapheme/word/line) and normalizes; it does not shape.
- **SheenBidi** resolves bidi runs; it does not shape.
- **HarfBuzz** maps text to glyph ids/positions; it does not rasterize.
- **FreeType** rasterizes glyphs into bitmaps; it does not shape.
- **ThorVG / Skia** rasterize paths; they do not own layout or the scene graph.
- **volk** only loads Vulkan entry points; it has no rendering behavior.
- **VMA** only allocates GPU memory; it does not record commands.
- **SPIRV-Reflect** reads shader binding metadata; it does not compile shaders.
- **Vulkan loader + ICD** are the actual GPU path; they stay dynamic ([ADR-012](../../decisions/ADR-012-static-linking.md)).

### Design-time: browser

```text
C++ engine -> [Emscripten] -> Wasm -> [browser WebGPU] -> <canvas>
```

- **Emscripten** is a toolchain; it is not shipped as a runtime library.
- **WebGPU bindings** (`webgpu.h` / emdawnwebgpu) call the browser's WebGPU.
- **Dawn** is the native WebGPU implementation; it is optional and used for native/headless
  parity tests, not linked into the Wasm viewer.

### Headless: CI and agent loop

```text
renderer -> [lavapipe | CPU rasterizer] -> RGBA buffer
         -> [stb_image_write | PNG encoder] -> PNG
         -> [stb_image decode + custom diff] -> report.json + diff image
```

- **lavapipe** is a software Vulkan ICD used by CI; never shipped to users.
- **stb_image_write** is the Phase 0 PNG writer; production may use a dedicated encoder.
- **stb_image** decodes PNGs for golden comparison.
- **Catch2** drives unit tests; **Google Benchmark** drives microbenchmarks. Neither is
  shipped.

### Platform, services, diagnostics

- **Win32 / X11 / Wayland / libxkbcommon / Cocoa** — window creation, input, lifecycle.
  GLFW/SDL3 are candidate abstractions over these.
- **libcurl / IXWebSocket / mbedTLS** — HTTP, WebSocket, TLS in `services/networking`.
- **sqlite3 / zstd** — local storage and compression in `services/storage`.
- **spdlog / Tracy / Crashpad** — logging, profiling, crash dumps in `diagnostics/`.
- **miniaudio** — optional audio hooks in `hal/audio`.
- **libpng / libjpeg-turbo / libwebp / lunasvg** — image decoding and SVG in `ui/image`.
- **mimalloc / glm / `{fmt}`** — optional allocator, math, formatting in `core/`.

### Dependency seams (what we depend on, and what we can swap)

| Dependency | Seam in Tessera | Why the seam exists |
|---|---|---|
| Yoga | `ui::LayoutEngine` | swap layout engine; hide Yoga types |
| FreeType | `ui::GlyphRasterizer` | swap rasterizer; keep text layout independent |
| HarfBuzz | `ui::TextShaper` | swap shaper; test with a fake |
| SheenBidi | `ui::BidiResolver` | isolate bidi policy |
| ICU | `ui::TextSegmenter`, `services::I18n` | make ICU optional/feature-gated |
| ThorVG / Skia | `rendering::PathRasterizer` | swap vector backend ([ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md)) |
| Vulkan / WebGPU | `rendering::RhiDevice` | two backends, one renderer ([ADR-003](../../decisions/ADR-003-thin-rhi-and-batched-2d-renderer.md)) |
| VMA / volk / SPIRV-Reflect | internal to `backend/vulkan` | never leak into UI code |
| own front end | `compiler::JsxParser` | parser is internal, not an ABI |
| nlohmann/json | `compiler::JsonCodec` | keep IR schema independent of the JSON library |
| clang-format | `compiler::Formatter` | tool, not a linked library |
| Win32 / X11 / Wayland / Cocoa | `hal::WindowSystem` | platform backends stay replaceable |
| libcurl | `services::HttpClient` | swap HTTP stack |
| spdlog | `diagnostics::Logger` | logging is not a hard dependency |
| Catch2 | test target only | never shipped |

### What is *not* a dependency

The scene graph, layout integration, CSS subset, renderer batching, game clock, dirty-flag
invalidation, IR, codegen, merger, golden runner, and the AI-agent feedback loop are our
code. They are the differentiators and are not delegated.

## Licensing

### Project license: MIT (decided 2026-11-07, ADR-013)

Tessera is released under the **MIT License** ([ADR-013](../../decisions/ADR-013-project-license-mit.md), project license is MIT). See [`LICENSE`](../../LICENSE) and
[`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md).

MIT obligations:

- Include the copyright notice and the permission notice in all copies or substantial
  portions of the software.
- For binary distributions, ship `THIRD_PARTY_NOTICES.md` (or the full notices file) and
  expose the MIT text plus third-party notices in an About/licenses UI.
- Apache-2.0 dependencies additionally require their `NOTICE` to be preserved.
- OFL font licenses, FreeType FTL notices, and Unicode/ICU notices must be retained.

MIT is compatible with every V1 dependency. It does not include an express patent grant; if
patent protection becomes a requirement, a superseding ADR can relicense the project.
Do not use GPL/AGPL for the core if the goal is broad adoption or closed-source embedding.

### License policy

Allowed in the shipped core:

- MIT, BSD-2-Clause, BSD-3-Clause, Apache-2.0, Boost Software License 1.0 (BSL-1.0),
  zlib, ISC, Unicode License, FreeType License (FTL), OFL-1.1 (fonts/assets), public domain.

Not allowed in the shipped core:

- GPL-2.0/3.0, AGPL-3.0, SSPL, Commons Clause, non-commercial, field-of-use restrictions,
  and any "source-available" license that is not OSI-approved.

Build-time-only GPL tools (e.g. `ccache`) are permitted because they are separate programs
and are not linked into or shipped with Tessera. Prefer `sccache` (Apache-2.0) where possible.

LGPL:

- **Not allowed in statically linked code** ([ADR-012](../../decisions/ADR-012-static-linking.md)). LGPL static linking requires
  relinkable object files and reverse-engineering permission.
- Permitted only for optional, dynamically loaded system integration that ships with the OS
  (e.g. AT-SPI via D-Bus), never as part of the static core.

### Compatibility rules

- **Apache-2.0 dependency in an MIT project:** allowed. Preserve the license text and
  the `NOTICE` file; state modifications to Apache-2.0 files.
- **MIT/BSD dependency in an MIT project:** allowed. Keep the copyright notice.
- **Static linking:** every permissive license above allows it with notice preservation.
  LGPL/GPL do not ([ADR-012](../../decisions/ADR-012-static-linking.md)).
- **Boost Software License 1.0 (BSL-1.0):** permissive. Do not confuse it with the Business
  Source License (also abbreviated BSL). Catch2 uses the Boost license.
- **OFL-1.1 fonts:** may be bundled; keep the license; do not rename modified fonts
  (Reserved Font Name); do not sell the fonts standalone.
- **FreeType:** use the FreeType License (BSD-style), not the GPL option.
- **Unicode/ICU:** include the Unicode License notice and the ICU copyright.
- **Public domain:** include attribution where the author asks for it (prefer stb's MIT
  option).
- **Transitive/bundled code:** Skia, Dawn, and Emscripten ship third-party code. Generate
  `NOTICE` / `THIRD_PARTY_NOTICES.md` from the actual build, not from a hand-written list.

### Specific dependency notes

| Dependency | License | Note |
|---|---|---|
| Vulkan SDK, loader, validation, `glslc` | Apache-2.0 | Keep notices; shader compiler is build-time |
| VMA, volk | MIT | Attribution in `NOTICE` |
| SPIRV-Reflect | Apache-2.0 | `NOTICE` |
| Yoga | MIT | `NOTICE` |
| ThorVG | MIT | `NOTICE` |
| Skia Graphite | BSD-3-Clause | Bundled third-party code; generate `NOTICE` |
| Dawn | BSD-3-Clause | Includes Tint and other third-party code; generate `NOTICE` |
| Emscripten | MIT / UIUC | Runtime notices in generated Wasm |
| FreeType | FTL (BSD-style) | Use FTL, not the GPL option |
| HarfBuzz | MIT | `NOTICE` |
| SheenBidi | Apache-2.0 | `NOTICE` |
| ICU | Unicode License v3 | `NOTICE` |
| libpng, libjpeg-turbo, libwebp | permissive | `NOTICE` |
| lunasvg | MIT | `NOTICE` |
| miniaudio | Public domain / MIT-0 | Attribution optional |
| libcurl | curl license | `NOTICE` |
| IXWebSocket | BSD-3-Clause | `NOTICE` |
| mbedTLS, OpenSSL | Apache-2.0 | `NOTICE` |
| libsodium | ISC | `NOTICE` |
| sqlite3 | Public domain | Optional attribution |
| zstd | BSD-3-Clause | `NOTICE` |
| spdlog | MIT | `NOTICE` |
| Tracy | BSD-3-Clause | `NOTICE` |
| Crashpad | Apache-2.0 | `NOTICE` |
| Breakpad | BSD-3-Clause | `NOTICE` |
| `{fmt}`, glm, mimalloc, rpmalloc | MIT | `NOTICE` |
| Catch2 | BSL-1.0 (Boost) | `NOTICE` |
| GoogleTest | BSD-3-Clause | `NOTICE` |
| Google Benchmark | Apache-2.0 | `NOTICE` |
| libFuzzer, clang-format | Apache-2.0 (LLVM exception) | `NOTICE` |
| nlohmann/json, Inja | MIT | Build-time |
| simdjson | Apache-2.0 | Build-time |
| CMake, vcpkg, Ninja, Node.js | permissive | Build-time |
| Python | PSF | Build-time |
| Noto Sans / Inter | OFL-1.1 | Test fonts; keep the license |

### Deferred / special cases

- **AT-SPI2 (Linux accessibility): LGPL-2.1.** It is a system component accessed over D-Bus;
  do not link it into the core. Keep accessibility optional and dynamically integrated.
- **libdbus: AFL-2.1 or GPL-2.0+.** Avoid linking; use the D-Bus protocol/system service, or a
  permissively licensed client if one is selected (no decision yet).
- **macOS Cocoa/AppKit and Windows Win32/UIA:** system frameworks/APIs; no redistribution
  issue.

### Compliance actions

1. Keep the MIT `LICENSE` at the repo root ([ADR-013](../../decisions/ADR-013-project-license-mit.md)).
2. Add `NOTICE` and `THIRD_PARTY_NOTICES.md`, generated from the resolved dependency graph.
3. Add SPDX headers to source files, e.g. `// SPDX-License-Identifier: MIT`.
4. Add a CI license check (`reuse lint`, ScanCode, or FOSSA) and an SBOM (SPDX/CycloneDX).
5. Keep `third_party/README.md` and this file in sync with the actual dependency graph.
6. Vendor code only with its license text; never strip copyright headers.
