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
4. **Pin everything.** Versions, hashes, fonts, lavapipe, shader compiler. Produce an SBOM.
5. **Determinism first.** No system fonts, no wall clock, no floating driver versions in
   golden tests (ADR-009).
6. **Watch the big four.** Skia (or ThorVG), ICU, Dawn, and Emscripten dominate build time
   and binary size; make them optional where possible.

## Status legend

- **V1 hard** — required for V1
- **V1 optional** — feature-gated, not needed for the first vertical slice
- **Build-time** — not shipped in the runtime
- **Candidate** — decision pending a spike
- **Deferred** — post-V1
- **Future** — long-term, not committed

## 1. GPU and rendering

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| Vulkan SDK (Headers, Loader, `glslc`, ValidationLayers) | V1 hard | Apache-2.0 | `rendering/backend/vulkan` | Vulkan API, loader, GLSL→SPIR-V, validation |
| VMA (VulkanMemoryAllocator) | V1 hard | MIT | `rendering/backend/vulkan` | GPU memory allocation |
| volk | V1 hard | MIT | `rendering/backend/vulkan` | Vulkan function loading |
| SPIRV-Reflect | V1 optional | Apache-2.0 | `rendering/backend/vulkan` | Shader reflection for pipeline layouts |
| stb_image_write | PoC only | Public domain / MIT | `tools/poc` | PNG output in the Phase 0 proof |
| stb_image | V1 test | Public domain / MIT | `tests/golden`, image decode | PNG/JPG decode for goldens and assets |
| ThorVG | Candidate (ADR-004) | MIT | `rendering/backend` | Vector paths, SVG |
| Skia Graphite | Candidate (ADR-004 fallback) | BSD-3-Clause | `rendering/backend` | Vector paths/text, Vulkan backend |
| Dawn | V1 hard for design-time (ADR-002) | BSD-3-Clause | `rendering/backend/webgpu` | WebGPU implementation for Wasm |
| Emscripten | V1 hard for design-time | MIT / UIUC | `designer/web` | C++ → WebAssembly toolchain |
| Mesa lavapipe | V1 CI | MIT | tests / CI | Software Vulkan ICD for deterministic goldens |
| Vulkan ValidationLayers | V1 debug | Apache-2.0 | dev / CI | Validation |
| naga | Future (ADR-011) | MIT / Apache-2.0 | `tools/shaders` | SPIR-V → WGSL |
| Tint | Future (ADR-011) | BSD-3-Clause | `tools/shaders` | WGSL → SPIR-V |
| Slang | Future (ADR-011) | Apache-2.0 (LLVM exception) | `tools/shaders` | Single-source shaders |
| SPIRV-Cross | Future | Apache-2.0 | `tools/shaders` | SPIR-V → HLSL/GLSL/MSL if needed |
| zlib | V1 transitive | zlib | `rendering/resources` | PNG/texture compression |

## 2. Layout

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| Yoga | V1 hard | MIT | `ui/layout` | Flexbox engine |
| taffy | Candidate | MIT | `ui/layout` | Grid engine via C ABI if we do not build a subset |
| — | — | — | `ui/style` | CSS parser/cascade is custom; no library |

## 3. Text

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| FreeType | V1 hard | FreeType License (BSD-style) | `ui/text` | Font loading and glyph rasterization |
| HarfBuzz | V1 hard | MIT | `ui/text` | Shaping: ligatures, kerning, complex scripts |
| SheenBidi | V1 hard | Apache-2.0 | `ui/text` | Bidirectional text |
| ICU | V1 hard | Unicode License | `ui/text`, `services/i18n` | Segmentation, normalization, locale, optional collation |
| libunibreak | V1 optional | zlib | `ui/text` | Line breaking if ICU is too heavy |
| Noto Sans / Inter (test fonts) | V1 test | OFL-1.1 | `tests/fixtures/fonts` | Deterministic golden text |

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
| tree-sitter + JSX/TSX grammars | Candidate | MIT | `compiler/analyzer` | Embeddable JSX/TSX parser (C) |
| SWC | Candidate | Apache-2.0 | `compiler/analyzer` | JSX/TSX parsing via Node/Rust |
| oxc | Candidate | MIT | `compiler/analyzer` | JSX/TSX parsing via Node/Rust |
| clang-format | V1 hard (toolchain) | Apache-2.0 (LLVM exception) | `compiler/codegen` | Deterministic formatting before merge (ADR-008) |
| nlohmann/json | V1 build-time | MIT | `compiler/ir`, manifest | IR/manifest JSON |
| simdjson | V1 optional | Apache-2.0 | `compiler` | Faster JSON if needed |
| Inja | V1 optional | MIT | `compiler/codegen` | Template engine; prefer plain string builders |
| CMake | V1 build | BSD-3-Clause | build | Build system |
| vcpkg | V1 build (recommended) | MIT | build | Dependency management |
| Ninja | V1 optional | Apache-2.0 | build | Fast builds |
| Node.js | Build-time if SWC/oxc | MIT | `compiler` | Host for JSX parser |
| Python 3 | Build-time | PSF | `scripts` | Tooling and CI helpers |

## 6. Platform and HAL

| Library / API | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| Win32 API | V1 (Windows) | System | `platform/windows` | Window, input, lifecycle |
| X11 | V1 (Linux) | MIT/X11 | `platform/linux` | Window, input |
| Wayland (`libwayland-client`, `wayland-protocols`) | V1 (Linux) | MIT | `platform/linux` | Window, input |
| libxkbcommon | V1 (Linux) | MIT | `hal/input` | Keyboard mapping |
| Cocoa / AppKit | Deferred (macOS, ADR-010) | System | `platform/macos` | Window, input |
| GLFW | Candidate (buy) | zlib | `hal/window` | Window/input abstraction |
| SDL3 | Candidate (buy) | zlib | `hal/window`, `hal/audio` | Window/input/audio abstraction |
| inotify / ReadDirectoryChangesW / FSEvents | V1 | System | `hal/filesystem` | File watching |
| Vulkan surface extensions | V1 | Apache-2.0 | `rendering/backend/vulkan` | `KHR_surface` + platform surfaces |
| UIA / AT-SPI / NSAccessibility | Deferred | System (AT-SPI: LGPL) | `ui/accessibility` | Accessibility |
| D-Bus / libdbus | Deferred | System | `ui/accessibility` | AT-SPI transport on Linux |

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
| Crashpad | V1 optional | Apache-2.0 | `diagnostics/crash` | Crash dumps |
| Breakpad | V1 optional alternative | BSD-3-Clause | `diagnostics/crash` | Crash dumps alternative |
| {fmt} | V1 optional | MIT | `core`, `diagnostics` | Formatting where `std::format` is insufficient |

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
| C++20 standard library | V1 hard | — | all | `pmr`, threads, filesystem, `format`, `expected` |
| glm | V1 optional | MIT | rendering / ui | Math library; we may keep a small 2D math header instead |
| mimalloc | V1 optional | MIT | `core/memory` | General allocator |
| rpmalloc | V1 optional alternative | MIT / Public domain | `core/memory` | Allocator alternative |

## Not dependencies (deliberate)

- **Qt, Slint, Flutter, Electron, Chromium/WebView** — the framework owns its renderer.
- **A React runtime in production** — production is pure C++ (AGENTS.md).
- **Platform text APIs for rendering** (DirectWrite, CoreText) — not cross-platform or
  deterministic (ADR-006).
- **A Vulkan-on-WebGPU shim** — design-time uses Dawn directly (ADR-002).
- **A general 3D engine** — the renderer is 2D-first (ADR-003).
- **A browser layout engine** — Yoga and a small Grid subset cover V1 (ADR-005).
- **GPL/LGPL libraries in the shipped core** — system-only access is allowed for deferred
  accessibility integration.

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
   replace with native backends later if needed.
2. **JSX parser:** tree-sitter (C, embeddable) vs SWC/oxc (Node/Rust, better React coverage).
3. **Vector paths:** ThorVG vs Skia Graphite (ADR-004 spike).
4. **Shaders:** GLSL + WGSL in parallel vs a single-source path via Slang/naga/Tint (ADR-011).
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
