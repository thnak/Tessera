# Setup and pin

> [<- Index](README.md) | [Path model and rasterizer seam ->](02-path-model-and-rasterizer.md)

## 1. Question and verdict

The spike had to answer four questions before ThorVG is pinned as a production dependency
([ADR-004](../../../decisions/ADR-004-buy-vector-path-rasterization.md)):

1. Can ThorVG be built reproducibly here, statically, with bounded size and build time?
2. Can it rasterize Tessera's path model deterministically into an RGBA8 buffer, so the output
   can be a golden and can be uploaded through the RHI?
3. Does ThorVG provide a Vulkan path, or is CPU rasterization the only viable V1 route?
4. Does the seam keep ThorVG out of public headers and out of UI code?

**Verdict: Go.** A meson-only upstream, a local CMake wrapper and a CPU rasterizer are enough for
V1. Nothing in the result depends on a Vulkan backend inside ThorVG, and the build/runtime cost is
small: ~12 s clean build of the ThorVG-dependent target, 894 KiB static archive (Release), 336 KiB
added to the spike executable, ~1 ms first render of a 512x512 icon/chart scene, 6.5 MiB peak
working set, and byte-identical output across runs and across Debug/Release. Skia Graphite is not
needed and stays the documented fallback only.


## 2. Dependency bring-up

### Pin

| Item | Value |
|---|---|
| Version | ThorVG **v1.1.3** (tag commit `589bc4120f2097fb1b135e552c2ef39f6581612b`) |
| Source | `https://github.com/thorvg/thorvg/archive/refs/tags/v1.1.3.tar.gz` |
| SHA-256 | `ba9957aeeadb4151d4486f11c638ae118c00210fa4906daa7b30143aa341e690` |
| License | MIT, copyright 2020 - 2026 ThorVG Project |
| Vendoring | **no**: fetched at configure time by `FetchContent`, verified by `URL_HASH` |

The pin lives in [`cmake/ThorVG.cmake`](../../../cmake/ThorVG.cmake); the two values must be updated
together. The archive was verified against a shallow clone of the tag: all selected sources and
`inc/` headers are byte-identical.

### Build wrapper (why not `FetchContent_MakeAvailable` alone)

ThorVG v1.1.3 is **meson-only**: the archive contains no `CMakeLists.txt` anywhere (verified), so
`FetchContent_MakeAvailable()` would only download and extract it. The wrapper in
`cmake/ThorVG.cmake` therefore compiles the pinned source set directly:

- sources: `src/common/**`, `src/renderer/**` minus `src/renderer/gpu_engine/**` (GL/WebGPU
  backends), `src/loaders/svg/**`, `src/loaders/raw/**` - 37 translation units;
- include paths: `inc/`, `src/common/`, `src/renderer/`, `src/renderer/cpu_engine/`,
  `src/loaders/raw/`, `src/loaders/svg/`, plus a generated directory holding `config.h`;
- `config.h` mirrors the meson feature options, with only the features the seam uses enabled:
  `THORVG_VERSION_STRING "1.1.3"`, `THORVG_CPU_ENGINE_SUPPORT`,
  `THORVG_SVG_LOADER_SUPPORT`, `THORVG_PARTIAL_RENDER_SUPPORT`, `THORVG_FILE_IO_SUPPORT`; every
  other `THORVG_*` feature stays undefined;
- `TVG_STATIC` (PUBLIC, selects the non-DLL API path), `NOMINMAX` on Windows, C++17 for the
  third-party sources, and `-w`/`/W0` so Tessera's `-Werror` policy does not apply to them.

An `ExternalProject` + meson build was rejected: it adds a build-time tool dependency (meson,
Python) without changing the produced artifact. A fork or vendored copy was rejected as well.
Nothing from ThorVG is vendored; the source tree lives in the CMake build directory only.

### Option

`TESSERA_ENABLE_THORVG` (root `CMakeLists.txt`) is **OFF** by default. When OFF:

- no download, no `tessera_thorvg` target, no ThorVG compile or link command;
- `tessera_path` still builds and provides the seam with its null backend, so non-ThorVG builds
  link;
- existing targets and binaries are unchanged. This was proven byte-for-byte: a Release build of
  three pre-existing targets (`merger_poc`, `tessera_schema_dump`, `image_diff_tests`) from the
  working tree with the option present and OFF produced identical SHA-256 hashes to the same
  targets built from a copy of the tree with the option/wrapper additions removed (link
  timestamps pinned with `-Wl,/Brepro` for the comparison; without that flag only the PE header
  timestamp differs).

When ON, `third_party/CMakeLists.txt` includes the wrapper and builds `tessera_thorvg`;
`nix`/CI users get the same target, no system ThorVG is used.
