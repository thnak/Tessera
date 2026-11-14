# Third-party notices

Tessera is licensed under the MIT License (see [`LICENSE`](LICENSE)).

This file lists third-party components and their licenses. The canonical dependency map with
modules, roles, and status is [`docs/specs/dependencies.md`](docs/specs/dependencies.md).

> This is the Phase 0/B1 set plus the planned V1 dependency set. It will be replaced by a
> generated file based on the resolved dependency graph. Do not remove copyright or license
> text from vendored code.

## In this repository (Phase 0)

### stb_image_write

- Files: `third_party/stb/stb_image_write.h`
- License: Public domain / MIT (dual)
- Copyright: Sean Barrett and contributors
- Upstream: https://github.com/nothings/stb

### stb_image

- Files: `third_party/stb/stb_image.h`
- License: Public domain / MIT (dual)
- Copyright: Sean Barrett and contributors
- Upstream: https://github.com/nothings/stb

### libjpeg-turbo

- Used by: `src/rendering/image/` (scaled JPEG decode, issue #14)
- Fetched at configure time from
  `https://github.com/libjpeg-turbo/libjpeg-turbo/archive/refs/tags/3.0.4.tar.gz` (pinned SHA-256);
  not vendored in the tree. Built as plain C without SIMD, TurboJPEG or tools
- License: BSD-3-Clause / IJG (the zlib-licensed SIMD code is not built)
- Copyright: libjpeg-turbo contributors, Independent JPEG Group
- Upstream: https://github.com/libjpeg-turbo/libjpeg-turbo (3.0.4)

### libwebp

- Used by: `src/rendering/image/` (WebP decode, issue #14)
- Fetched at configure time from
  `https://github.com/webmproject/libwebp/archive/refs/tags/v1.4.0.tar.gz` (pinned SHA-256);
  not vendored in the tree. Only libwebp, libwebpdemux and sharpyuv are built
- License: BSD-3-Clause
- Copyright: Google LLC
- Upstream: https://github.com/webmproject/libwebp (1.4.0)

### Vulkan SDK components (Vulkan-Headers, Vulkan-Loader, Vulkan-ValidationLayers, shaderc/glslc)

- Used by: `src/rendering/backend/vulkan/`, build step
- License: Apache-2.0
- Copyright: The Khronos Group Inc. and contributors
- Upstream: https://github.com/KhronosGroup/Vulkan-Headers,
  https://github.com/KhronosGroup/Vulkan-Loader,
  https://github.com/KhronosGroup/Vulkan-ValidationLayers,
  https://github.com/google/shaderc

### Vulkan Memory Allocator (VMA)

- Files: `third_party/vma/vk_mem_alloc.h`
- Used by: `src/rendering/backend/vulkan/`
- License: MIT
- Copyright: Advanced Micro Devices, Inc. and contributors
- Upstream: https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator

### volk

- Files: `third_party/volk/volk.h`, `third_party/volk/volk.c`
- Used by: `src/rendering/backend/vulkan/`
- License: MIT
- Copyright: Arseny Kapoulkine and contributors
- Upstream: https://github.com/zeux/volk

### Yoga

- Files: `third_party/yoga/yoga/`
- Used by: `src/ui/layout/`
- License: MIT
- Copyright: Meta Platforms, Inc. and affiliates
- Upstream: https://github.com/facebook/yoga (v3.2.2)

### FreeType

- Used by: `src/ui/text/`
- Fetched at configure time from
  `https://github.com/freetype/freetype/archive/refs/tags/VER-2-13-3.tar.gz` (pinned SHA-256);
  not vendored in the tree
- License: FreeType License (FTL, BSD-style); the GPL option is not used
- Copyright: The FreeType Project and contributors
- Upstream: https://github.com/freetype/freetype (2.13.3)

### HarfBuzz

- Used by: `src/ui/text/`
- Fetched at configure time from
  `https://github.com/harfbuzz/harfbuzz/archive/refs/tags/10.2.0.tar.gz` (pinned SHA-256);
  not vendored in the tree
- License: MIT
- Copyright: HarfBuzz contributors
- Upstream: https://github.com/harfbuzz/harfbuzz (10.2.0)

### ThorVG

- Used by: `src/rendering/path/` (PathRasterizer implementation)
- Fetched at configure time from
  `https://github.com/thorvg/thorvg/archive/refs/tags/v1.1.3.tar.gz` (pinned SHA-256);
  not vendored in the tree
- License: MIT
- Copyright: 2020 - 2026 ThorVG Project
- Upstream: https://github.com/thorvg/thorvg (v1.1.3)

### Noto Sans (test font)

- Files: `tests/fixtures/fonts/NotoSans-Regular.ttf`, `tests/fixtures/fonts/NotoSansHebrew-Regular.ttf`, `tests/fixtures/fonts/OFL.txt`
- Used by: deterministic unit and golden tests
- License: SIL Open Font License 1.1 (OFL-1.1)
- Copyright: The Noto Project Authors
- Upstream: https://github.com/notofonts/noto-fonts

### Twemoji Mozilla (emoji font)

- Files: `tests/fixtures/fonts/Twemoji.Mozilla.ttf` (v0.7.0, COLRv0), `tests/fixtures/fonts/TWEMOJI-LICENSE.txt`
- Used by: `emoji_font` test; the intended emoji face at the end of a font stack
- License: CC-BY 4.0 (graphics, Twitter, Inc and other contributors); MIT (font build, Mozilla)
- Attribution: "Twemoji" graphics copyright Twitter, Inc and other contributors, licensed under CC-BY 4.0; COLRv0 build by Mozilla
- Upstream: https://github.com/mozilla/twemoji-colr

### Mesa (lavapipe)

- Used by: CI only; not shipped in an application
- License: MIT
- Copyright: Mesa contributors
- Upstream: https://gitlab.freedesktop.org/mesa/mesa

### libX11 (system library)

- Used by: `src/platform/linux/` (X11 window), `src/rendering/backend/vulkan/` (Xlib surface)
- License: MIT/X11
- Copyright: X.Org Foundation and contributors
- Upstream: https://gitlab.freedesktop.org/xorg/lib/libx11
- Linked dynamically from the operating system when X11 is found at configure time; not
  vendored or redistributed with Tessera.

## Planned V1 dependencies

| Component | License | Copyright / upstream |
|---|---|---|
| SPIRV-Reflect | Apache-2.0 | Google / Khronos |
| Perfetto SDK | Apache-2.0 | Google |
| AndroidX WindowManager | Apache-2.0 | Google / Android Open Source Project |
| Yoga | MIT | Meta Platforms, Inc. |
| FreeType | FreeType License (BSD-style) | The FreeType Project |
| HarfBuzz | MIT | HarfBuzz contributors |
| SheenBidi | Apache-2.0 | Tejus Adiga |
| ICU | Unicode License v3 | Unicode, Inc. / ICU contributors |
| ThorVG | MIT | ThorVG Project |
| Skia (fallback) | BSD-3-Clause | Google LLC |
| Dawn | BSD-3-Clause | Google LLC |
| Emscripten | MIT / UIUC | Emscripten contributors |
| emdawnwebgpu (Emscripten port, `v20260423.175430`) | MIT / UIUC (JS bindings), BSD-3-Clause (Dawn headers) | Emscripten contributors; The Dawn & Tint Authors; linked into the Wasm WebGPU build only |
| CodeMirror 6 | MIT | Marijn Haverbeke and contributors; bundled in the hosted studio page |
| fflate | MIT | Arjun Barrett; hosted studio zip import/export |
| Catch2 | Boost Software License 1.0 | Catch2 contributors |
| Google Benchmark | Apache-2.0 | Google LLC |
| libpng | libpng License | PNG Reference Library authors |
| libjpeg-turbo | BSD-3-Clause / IJG | libjpeg-turbo contributors |
| libwebp | BSD-3-Clause | Google LLC |
| zlib | zlib License | Jean-loup Gailly and Mark Adler |
| libcurl | curl License | Daniel Stenberg and contributors |
| IXWebSocket | BSD-3-Clause | Machine Zone, Inc. |
| mbedTLS | Apache-2.0 | ARM Limited |
| OpenSSL | Apache-2.0 | The OpenSSL Project |
| libsodium | ISC | Frank Denis and contributors |
| sqlite3 | Public domain | SQLite authors |
| zstd | BSD-3-Clause | Meta Platforms, Inc. |
| spdlog | MIT | Gabi Melman and contributors |
| Tracy | BSD-3-Clause | Bartosz Taudul |
| Crashpad | Apache-2.0 | Google LLC |
| {fmt} | MIT | Victor Zverovich |
| glm | MIT | G-Truc Creation |
| mimalloc | MIT | Microsoft Corporation |
| nlohmann-json / Inja | MIT | their respective authors |
| simdjson | Apache-2.0 | their respective authors |
| libFuzzer | Apache-2.0 with LLVM exception | LLVM Project |
| Noto Sans / Inter | SIL Open Font License 1.1 | their respective authors |
| Twemoji Mozilla | CC-BY 4.0 (graphics), MIT (build) | Twitter, Inc and contributors; Mozilla |
| libunibreak | zlib | Wu Yongwei and libunibreak contributors |
| lunasvg | MIT | lunasvg contributors |
| miniaudio | Public domain / MIT-0 | David Reid (mackron) |
| Breakpad | BSD-3-Clause | Google LLC / Breakpad authors |
| rpmalloc | MIT / Public domain | Mattias Jansson |

## Build-time tools (not linked or shipped)

| Tool | License | Notes |
|---|---|---|
| clang-format / clang-tidy | Apache-2.0 with LLVM exception | formatting and linting |
| CMake | BSD-3-Clause | build system |
| Ninja | Apache-2.0 | build tool |
| sccache | Apache-2.0 | optional compiler cache (preferred) |
| ccache | GPL-3.0-or-later | optional compiler cache; separate program, not linked |
| Node.js | MIT | hosted-studio build scripts and the `tessera-mcp` stdio server; design-time tooling, never linked into the app |
| SPIRV-Cross, SPIRV-Tools (spirv-val, spirv-opt) | Apache-2.0 | from the pinned Vulkan SDK; only with `TESSERA_SHADER_CROSS=ON` (ADR-049); output stays in the build directory |
| DirectX Shader Compiler (dxc) | University of Illinois/NCSA | from the pinned Vulkan SDK; only validates generated HLSL under `TESSERA_SHADER_CROSS=ON` |
| naga-cli 30.0.1 | MIT OR Apache-2.0 | `cargo install naga-cli --version 30.0.1 --locked` (pin in `tools/naga-version.txt`); `TESSERA_SHADER_CROSS=ON` and the WebGPU CI lane; SPIR-V to WGSL and WGSL validation |
| emsdk 6.0.13 (Emscripten SDK manager, bundled Node.js and Python) | MIT | installs the pinned Emscripten (`tools/emsdk-version.txt`) outside the repository for the `wasm-webgpu` preset and the WebGPU CI lane |

## Not linked

- **AT-SPI / libatspi / glib** (LGPL-2.1): not linked, because static linking is a
  requirement (ADR-012). Linux accessibility is expected to use the D-Bus protocol or an
  optional runtime-only integration.
- **libdbus** (AFL-2.1 / GPL-2.0+): not linked.
- **GPL/AGPL components**: not allowed in the shipped core.

## Icon sets (imported at build time, not linked)

Iconify sets are content, not a linked dependency: `tessera icons add` reads a **pinned on-disk copy**
of a set, and only the icons an app references are converted and embedded. Assets are never fetched
from the Iconify API at run time, and the tool never opens a network connection
(ADR-047 item 8, [`docs/design/component-library.md`](docs/design/component-library.md)).

- Accepted licenses: MIT, ISC, Apache-2.0, BSD-2-Clause/BSD-3-Clause/0BSD, CC0-1.0, OFL-1.1.
- CC-BY sets are accepted and add an attribution block to the generated `ICON_LICENSES.txt`.
- Share-alike sets (CC-BY-SA) are accepted only with an explicit `--allow-share-alike`.
- GPL-family, non-commercial, weak-copyleft and unknown licenses are rejected, and the message names
  the set, the license as recorded and the reason (`tools/icons/src/icon_license.cpp`).
- The generated attribution for an import is `ICON_LICENSES.txt` next to the generated
  `icons.manifest` and `icons.g.hpp`. The repository's own end-to-end example, generated from the
  pinned fixture sets, is checked in at `tests/golden/icons/basic/ICON_LICENSES.txt`.

## Updating this file

1. Add the dependency to `docs/specs/dependencies.md` first.
2. Record the license and copyright holder here.
3. In Big Step B, replace this hand-written file with a generator that reads the resolved
   dependency graph (vcpkg/CMake) and emits the notices automatically.
