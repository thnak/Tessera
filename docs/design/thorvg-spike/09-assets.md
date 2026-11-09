# Embedded asset bundles

Status: implemented (`src/ui/assets/`, `cmake/embed_assets.cmake`, `tessera_embed_assets`). SVG
assets ship inside the statically linked binary so the runtime never needs a filesystem or a resource
path, which is the [static-linking requirement](../../../decisions/ADR-012-static-linking.md). The
bundle feeds the existing decode/cache pipeline from
[SVG decode](07-svg-decode.md) and
[document cache](08-document-cache.md); it adds no new renderer path.

Layer note: `AssetBundle` itself is UI-agnostic (`tessera::ui::assets`, no rendering dependency).
The SVG-on-bundle helper (`load_asset_svg`, `load_asset_svg_texture`) lives in the same service
because it composes the app-facing bundle with the rendering path pipeline; UI depends on
rendering, never the other way around.

## API

[`asset_bundle.hpp`](../../../include/tessera/ui/assets/asset_bundle.hpp):

- `AssetBundle` is a read-only view over a sorted `AssetEntry` table plus one contiguous data blob:
  `find(name) -> std::expected<Asset, AssetError>` (`not_found` / `invalid_name`),
  `contains`, `entries()`, `size()`, `empty()`, `byte_size()`.
  `Asset { name, mime, std::span<const std::byte> bytes }`.
- Lookup is a `std::lower_bound` binary search over the normalized names: no allocation, no RTTI,
  no copying, and the returned span points into the binary's read-only data.
- `normalize_asset_name()` is defined once in the header and shared by the runtime and the
  generator:
  - `/` is the only separator; a single optional leading `/` is stripped; `\` is invalid;
  - no empty segments (`//`), no `.` or `..` segments, no trailing `/`;
  - allowed characters `A-Z a-z 0-9 . _ - /`;
  - lookup is **case-sensitive**; a differently-cased name is `not_found`, a malformed one is
    `invalid_name`.

[`asset_svg.hpp`](../../../include/tessera/ui/assets/asset_svg.hpp):

- `load_asset_svg(bundle, name, SvgDocumentCache&, SvgLimits = {})` returns
  `std::expected<const SvgDocument*, AssetSvgError>`. The asset content hash keys the document
  cache, so two names for identical bytes share one decode.
- `load_asset_svg_texture(bundle, name, SvgDocumentCache&, PathTextureCache&, scale_milli = 1000,
  limits = {})` is the document → texture convenience composition (one texture per asset/size/scale,
  inheriting the texture cache's LRU, D2 format rule and D7 handling). The texture key uses the
  content hash as the document identity, so a changed asset is automatically a new key.
- `AssetSvgError` mirrors `SvgError` (`malformed`, `unsupported`, `input_too_large`,
  `limit_exceeded`, `backend_unavailable`, `backend_failure`) and adds `not_found`,
  `invalid_name`, `texture_failed`, `device_lost`. A missing asset is `not_found`, never a crash; a
  failed decode is never cached.

## Manifest format

A manifest is a small text file (LF or CRLF; comments and blank lines allowed):

```text
# SPDX-License-Identifier: MIT (hand-authored)
icons/app.svg = ../svg/app.svg
meta/readme.txt = readme.txt
```

- One `normalized/name = path` per line; the path is resolved **relative to the manifest file**
  (absolute paths are accepted but the generated output never embeds a path).
- `#` starts a comment line; inline comments are not supported; duplicate normalized names are a
  loud generator error.
- The MIME hint is derived from the extension (`.svg` → `image/svg+xml`, `.txt` → `text/plain`,
  `.json`, `.png`, `.jpg/.jpeg`, `.webp`; everything else is `application/octet-stream`).
- Files are embedded exactly as bytes; no compression, no text normalization.

## Generator (CMake script, runs on the host)

Asset generation is a **CMake script**, [`cmake/embed_assets.cmake`](../../../cmake/embed_assets.cmake),
executed through `${CMAKE_COMMAND} -P` by the CMake helper. It is not a target-built executable:
in a cross-compiling build (Android NDK, iOS SDK) a target binary cannot run on the build host
("Exec format error"/"not found"), so **generators must run on the host, never execute target
binaries**. CMake is always available and host-independent; `file(READ ... HEX)` plus a small
string formatter produces the arrays, and the output is written with `configure_file(... NEWLINE_STYLE
UNIX)` so Windows hosts also emit LF.

```text
cmake -DMANIFEST=icons.manifest -DNAMESPACE=tessera::ui::assets::app \
      -DHEADER=<build>/app_assets.hpp -DSOURCE=<build>/app_assets.cpp \
      -P cmake/embed_assets.cmake
```

- Output is deterministic: entries are sorted by normalized name, the data blob is the
  concatenation in that same order, hex bytes are lowercase and fixed-width, lines use LF, and the
  file embeds only the manifest **basename** (no timestamps, no absolute paths, no locale-dependent
  formatting). Two runs from different working directories are byte-identical (CTest
  `asset_generator`), and the CMake-script output is byte-identical to the previous C++ generator
  except for the two provenance-comment lines (the checked-in golden was regenerated for that).
- The header exposes `kAssetCount`, `kAssetBytes` and `bundle()` in the requested namespace; the
  source defines a `constexpr unsigned char` data array and the `AssetEntry` table in read-only
  memory.
- Duplicate normalized names, invalid names and missing files fail the script loudly (non-zero
  `cmake -P` exit, so the custom command fails).
- The manifest is re-parsed by the CMake helper at configure time to record every asset file as a
  build dependency; touching an asset re-runs the script.
- The agent-facing golden is `tests/golden/assets/assets.hpp` + `assets.cpp` (generated from
  `tests/fixtures/assets/icons.manifest` with namespace `tessera::ui::assets::golden`); regenerate it
  deliberately via `asset_gen_test.cmake -DWRITE_GOLDEN=ON` (command in the test).

## CMake helper

```cmake
include(${PROJECT_SOURCE_DIR}/cmake/TesseraAssets.cmake)
tessera_embed_assets(my_app
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/assets.manifest
    NAMESPACE tessera::ui::assets::my_app)
```

- Adds the generated source to `my_app`, puts the generated header on its private include path and
  wires the embed script, the manifest and every manifest file as build dependencies (a missing
  asset file is a configure error; touching an asset regenerates the bundle).
- No generator target is required, so **native and cross-compiling builds use the same mechanism**;
  a zero-asset manifest still compiles and links (`asset_zero_bundle` test).
- No extra root CMake lines are needed for a consumer beyond the one `add_subdirectory(src/ui/assets)`
  line this slice adds.

## Memory cost

Asset bytes live in the binary's read-only data. For the three test fixtures (two SVGs and a text
file):

| Measurement | Value |
|---|---|
| Embedded payload (`kAssetBytes`) | 542 B |
| Generated header / source on disk (build time only, not shipped) | 644 B / 4,268 B |
| Runtime per-entry overhead (`AssetEntry`: 2 `string_view` + offset/size) | ~40 B per asset |

The generated source is roughly 8x the payload because bytes are emitted as hex text, but that text
is deleted after compilation; only the packed `unsigned char` arrays and the entry table are
linked. The static-link/license gates are unaffected: no new dependency, the bytes are project-owned
fixtures.

## Tests

- `asset_bundle` (`tests/unit/asset_bundle_tests.cpp`, registered from `src/ui/assets/CMakeLists.txt`
  with a generated bundle): lookup hit/miss, case sensitivity, leading-slash normalization, invalid
  names, sorted/unique table, MIME hints, byte equality with the source file, and
  `load_asset_svg`/`load_asset_svg_texture` producing exactly the same document and texture bytes as
  decoding the file from disk (FNV compared); ThorVG OFF returns `backend_unavailable`.
- `asset_zero_bundle`: a manifest with no assets compiles, links and reports `not_found` for every
  name.
- `asset_generator` (CMake script): byte-identical output across two runs from different working
  directories, checked-in golden comparison, duplicate-name rejection, invalid-name rejection (the
  same host-side mechanism cross-compiling builds use, so this test covers it).

## Deferrals

- Compression/packing and content-hash dedupe of identical assets (the current bundle is a flat
  concatenation; identical bytes appear twice).
- Hot reload / dev-time asset watching (the bundle is compiled in; `SvgDocumentCache` exposes
  `invalidate()` for a future owner).
- Encrypted or license-restricted assets, and image formats beyond SVG (the bundle is byte-agnostic;
  decoding PNG/JPEG needs the image stack, which is out of this slice).
- Per-asset metadata beyond name/size/mime (e.g. source path or license tag).
