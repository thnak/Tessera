# Build, static linking, and packaging

Status: current B1 (renderer-core phase of Big Step B) CMake/presets; packaging is a target of [Big Step F](../../AGENTS.md#big-steps-group-work-dont-micro-step) (production breadth). [ADR-012](../../decisions/ADR-012-static-linking.md) (static linking) and [ADR-013](../../decisions/ADR-013-project-license-mit.md) (MIT license) are normative.
Updates/signing/notarization: [`updates-and-distribution-v1.md`](../specs/updates-and-distribution-v1.md).

## 1. Build system

- CMake >= 3.24, C++23, Ninja preferred; Visual Studio generators supported.
- `CMakePresets.json`: `dev`, `release`, `ci`, `vs2026`, `vs2022`.
- Options:
  - `BUILD_SHARED_LIBS=OFF` (default)
  - `TESSERA_STATIC_RUNTIME=ON` (default)
  - `TESSERA_USE_COMPILER_CACHE=ON` (auto-detect sccache/ccache)
  - `TESSERA_BUILD_POC=ON`, `TESSERA_BUILD_TESTS=ON`, `TESSERA_BUILD_VULKAN=ON`
- `compile_commands.json` exported for clang-tidy/editors.

Commands:

```bash
cmake --preset dev && cmake --build --preset dev && ctest --preset dev
cmake --preset ci  && cmake --build --preset ci  && ctest --preset ci
```

```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
```

## 2. Targets

| Target | Kind | Notes |
|---|---|---|
| `tessera_rhi` | interface (headers only) | RHI (render hardware interface) interfaces |
| `tessera_gpu` | static | batched 2D renderer |
| `tessera_rhi_null` | static | null backend |
| `tessera_scene` | static | scene graph/events |
| `tessera_merger` | static | protected-region merger |
| `tessera_vulkan` | static | offscreen Vulkan renderer |
| `tessera_rhi_vulkan` | static | Vulkan RHI backend |
| `tessera_stb` | static | stb image write |
| demos/tests | exe | PoC/tests |

Runtime libraries are static; OS/GPU system components remain dynamic ([ADR-012](../../decisions/ADR-012-static-linking.md)).

## 3. Static linking

- MSVC: `/MT` (`MultiThreaded$<$<CONFIG:Debug>:Debug>`).
- GCC/Clang: `-static-libstdc++ -static-libgcc` where supported.
- `BUILD_SHARED_LIBS=OFF`; no plugin ABI.
- System dynamic components: Vulkan loader/ICDs (installable client drivers), Win32/X11/Wayland/Cocoa/Android frameworks,
  glibc (unless musl), GPU drivers.
- No GPL/LGPL in statically linked code. Linux accessibility uses AT-SPI (Assistive Technology Service Provider Interface) over D-Bus (Linux message bus), not
  libatspi/glib. libdbus is not linked.
- ICU (International Components for Unicode) data is embedded (`--with-data-packaging=static` or `udata_setCommonData`).
- CI checks the link graph (`ldd`/`dumpbin`/`otool`) for unexpected third-party dynamic libs.

### Static runtime proof

ADR-012 is proven with a real Release build, not only by inspecting whatever artifact already
exists:

```bash
python scripts/check_static_runtime.py --config Release
```

The script configures the throwaway directory `build/static-runtime-proof` with
`TESSERA_STATIC_RUNTIME=ON`, `CMAKE_BUILD_TYPE=Release` and `TESSERA_BUILD_VULKAN=OFF`, builds the
small `merger_tests` target, and runs `scripts/check_static_link.py` on the executable (same
per-OS allowlist and forbidden-runtime rule as CI). On Windows it bootstraps the MSVC environment
through `vcvars64.bat` when `cl`/`nmake` are not on `PATH`. Exit codes: 0 pass (or Debug
SKIPPED), 1 static-link violation, 2 tool/setup failure. A thin
`scripts/check_static_runtime.ps1` wrapper forwards the same arguments.

What it proves: the Release artifact imports no dynamic MSVC runtime (`VCRUNTIME*`, `MSVCP*`,
`ucrtbase.dll`/`api-ms-win-crt-*`), no dynamic `libstdc++`/`libgcc_s`, and no third-party dynamic
library outside the per-OS allowlist — the flags above reach a linked framework executable.

What it does not prove: that every shipped target uses the same flags (one representative target
is checked), that the Vulkan loader/ICDs, glibc, X11/Wayland, Win32 or macOS frameworks are static
(they stay dynamic by design), or that static third-party archives are license-clean — that is
`scripts/check_dependencies.py`, which checks notices/licenses rather than link flags. Debug
builds use the debug C/C++ runtime (`/MTd`/`MDd`), so the script reports `SKIPPED` for Debug
instead of passing or failing; it applies to Release only.

## 4. Shaders

- GLSL -> SPIR-V at build time via `glslc --target-env=vulkan1.3 -mfmt=c` (generated header).
- Never commit `.spv`.
- WebGPU uses WGSL (WebGPU shading language; SPIR-V is Vulkan's shader bytecode, see [glossary](../reference/glossary.md)); shared interface described by the shader manifest ([`shader-manifest-v1.md`](../specs/shader-manifest-v1.md)).
- Shader compiler version pinned; shader changes require smoke/golden tests.

## 5. Assets and fonts

- Fonts/images/themes are committed or packaged with the app.
- Asset manifest contains ids, paths, hashes, and licenses.
- ICU data, font licenses (OFL/FTL), and image licenses are shipped.
- Deterministic goldens (reference images, see [glossary](../reference/glossary.md)) use committed fonts only.
- Large assets may be streamed; packaging records the asset generation/hash.

## 6. Notices and licenses

- `THIRD_PARTY_NOTICES.md` is the canonical shipped notice source.
- Package includes a `THIRD_PARTY_NOTICES.txt` and an About/licenses UI.
- Preserve Apache-2.0 NOTICE, MIT/BSD copyright, OFL/FTL, and ICU/Unicode notices.
- New dependency -> update `docs/specs/dependencies.md` and `THIRD_PARTY_NOTICES.md`.
- CI checks that notices cover the link graph.

## 7. Windows packaging (target)

- One `.exe` + assets directory (or embedded resources).
- Static CRT; Vulkan loader/ICD system components remain dynamic.
- Code signing optional but recommended; version resources/manifest.
- Validate on a clean Windows VM without the Vulkan SDK installed (drivers only).
- Crash dumps/PDBs (program database debug-symbol files): ship symbols separately; do not ship debug binaries.

## 8. Linux packaging (target)

- Single executable + assets; glibc/system libs dynamic.
- If a fully static Linux binary is required, evaluate musl; GPU drivers still dynamic.
- AppImage/Flatpak/tarball are packaging options; document sandbox/GPU access.
- Strip symbols; ship separate debug symbols.
- Verify lavapipe (Mesa software Vulkan driver, see [glossary](../reference/glossary.md))/software fallback and validation-clean debug builds.

## 9. Reproducibility and SBOM

- Pin dependency versions/hashes, shader compiler, lavapipe, fonts.
- Avoid timestamps/absolute paths in generated code and binaries; the exceptions that cannot be avoided are not enumerated.
- Produce an SBOM (software bill of materials; format not decided, see section 12) and a notice bundle.
- CI artifact: version, commit, toolchain, dependency lock, test results.
- Deterministic build flags (`-ffile-prefix-map`, no `__DATE__`/`__TIME__`).

## 10. CI

Current Linux CI: format check, configure/build, static runtime check, ctest with lavapipe,
Vulkan offscreen smoke, artifact upload.

Target additions (no Big Step assigned): Windows build/test, static link graph check, notices/SBOM check, golden
tests, fuzz job, performance job.

## 11. Release checklist

- [ ] version/SemVer updated; changelog written
- [ ] configure/build/tests on Windows + Linux
- [ ] validation layers clean (debug)
- [ ] static link graph allowlist passes
- [ ] production binary contains no JS/Wasm/React runtime or dynamic plugin loader ([ADR-024](../../decisions/ADR-024-native-first-boundaries.md), native-first boundaries)
- [ ] bridge contract/leak tests pass (JNI = Java Native Interface, ObjC = Objective-C, IPC = inter-process communication; see [`bridges.md`](bridges.md))
- [ ] notices/SBOM updated and shipped
- [ ] assets/fonts/ICU data packaged
- [ ] clean-VM smoke test (no SDK)
- [ ] crash symbols archived
- [ ] docs/ADRs updated
- [ ] golden baselines reviewed (if changed)

## 12. Open questions

- SBOM format/tooling (no decision yet).
- Installer vs portable archive per platform.
- Code signing/notarization policy: see
  [`updates-and-distribution-v1.md`](../specs/updates-and-distribution-v1.md).
- Whether to ship a software fallback renderer.
- musl fully static Linux build.
- Update mechanism/signature verification: see
  [`updates-and-distribution-v1.md`](../specs/updates-and-distribution-v1.md).
