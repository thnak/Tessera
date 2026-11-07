# Third-party notices

Tessera is licensed under the MIT License (see [`LICENSE`](LICENSE)).

This file lists third-party components and their licenses. The canonical dependency map with
modules, roles, and status is [`docs/specs/dependencies.md`](docs/specs/dependencies.md).

> This is the Phase 0 set plus the planned V1 dependency set. In Big Step B it will be
> replaced by a generated file based on the resolved dependency graph. Do not remove
> copyright or license text from vendored/vendored-adjacent code.

## In this repository (Phase 0)

### stb_image_write

- Files: `third_party/stb/stb_image_write.h`
- License: Public domain / MIT (dual)
- Copyright: Sean Barrett and contributors
- Upstream: https://github.com/nothings/stb

### Vulkan SDK components (Vulkan-Headers, Vulkan-Loader, Vulkan-ValidationLayers, shaderc/glslc)

- Used by: `src/rendering/backend/vulkan/`, build step
- License: Apache-2.0
- Copyright: The Khronos Group Inc. and contributors
- Upstream: https://github.com/KhronosGroup/Vulkan-Headers,
  https://github.com/KhronosGroup/Vulkan-Loader,
  https://github.com/KhronosGroup/Vulkan-ValidationLayers,
  https://github.com/google/shaderc

### Mesa (lavapipe)

- Used by: CI only; not shipped in an application
- License: MIT
- Copyright: Mesa contributors
- Upstream: https://gitlab.freedesktop.org/mesa/mesa

## Planned V1 dependencies

| Component | License | Copyright / upstream |
|---|---|---|
| VMA | MIT | AMD / GPUOpen |
| volk | MIT | Arseny Kapoulkine |
| SPIRV-Reflect | Apache-2.0 | Google / Khronos |
| Yoga | MIT | Meta Platforms, Inc. |
| FreeType | FreeType License (BSD-style) | The FreeType Project |
| HarfBuzz | MIT | HarfBuzz contributors |
| SheenBidi | Apache-2.0 | Tejus Adiga |
| ICU | Unicode License v3 | Unicode, Inc. / ICU contributors |
| ThorVG | MIT | ThorVG contributors |
| Skia (fallback) | BSD-3-Clause | Google LLC |
| Dawn | BSD-3-Clause | Google LLC |
| Emscripten | MIT / UIUC | Emscripten contributors |
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
| tree-sitter / oxc / nlohmann-json / Inja | MIT | their respective authors |
| SWC / simdjson | Apache-2.0 | their respective authors |
| libFuzzer | Apache-2.0 with LLVM exception | LLVM Project |
| Noto Sans / Inter | SIL Open Font License 1.1 | their respective authors |

## Build-time tools (not linked or shipped)

| Tool | License | Notes |
|---|---|---|
| clang-format / clang-tidy | Apache-2.0 with LLVM exception | formatting and linting |
| CMake | BSD-3-Clause | build system |
| Ninja | Apache-2.0 | build tool |
| sccache | Apache-2.0 | optional compiler cache (preferred) |
| ccache | GPL-3.0-or-later | optional compiler cache; separate program, not linked |

## Not linked

- **AT-SPI / libatspi / glib** (LGPL-2.1): not linked, because static linking is a
  requirement (ADR-012). Linux accessibility is expected to use the D-Bus protocol or an
  optional runtime-only integration.
- **libdbus** (AFL-2.1 / GPL-2.0+): not linked.
- **GPL/AGPL components**: not allowed in the shipped core.

## Updating this file

1. Add the dependency to `docs/specs/dependencies.md` first.
2. Record the license and copyright holder here.
3. In Big Step B, replace this hand-written file with a generator that reads the resolved
   dependency graph (vcpkg/CMake) and emits the notices automatically.
