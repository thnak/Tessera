# Packaging and release guide

Status: target for Big Step F (production breadth; see [AGENTS.md, Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Update/signing/rollback:
[`updates-and-distribution-v1.md`](../specs/updates-and-distribution-v1.md). Big Step B1 (core renderer, complete) builds demos and tests only, not a shipped app. V1 means the first-release scope defined in AGENTS.md. Build design: [`build-and-packaging.md`](../design/build-and-packaging.md).

## 1. Packaging model

- Statically link Tessera and third-party C/C++ libraries into one executable ([ADR-012](../../decisions/ADR-012-static-linking.md), static linking).
- Keep system components dynamic: Vulkan loader and ICDs (installable client drivers, the GPU drivers), OS frameworks, glibc (unless the Linux build uses musl, which is an open question, see section 12).
- Ship assets (fonts/images/themes) and notices alongside the executable or embedded.
- No plugin ABI (binary interface for third-party native modules) or hot-loadable native modules in V1.

## 2. Build

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
```

Use `TESSERA_STATIC_RUNTIME=ON` (default) and `BUILD_SHARED_LIBS=OFF`.

## 3. Link-graph verification

| Platform | Command | Expectation |
|---|---|---|
| Windows | `dumpbin /dependents app.exe` | only system DLLs (for example kernel32, user32, vulkan-1) |
| Linux | `ldd app` / `readelf -d app` | no libstdc++/libgcc_s; system libs only |
| macOS | `otool -L app` | system frameworks only |

Unexpected third-party dynamic libraries fail the release.

## 4. Assets

- Fonts: committed files; ship OFL/FTL licenses.
- Images: ship licenses/attribution; validate decode limits.
- Themes: JSON tokens; ship defaults.
- ICU data: embedded static data (no `icudt*.dat` sidecar).
- Shaders: built into the binary; no runtime shader compilation from user data.
- Asset manifest: ids, hashes, licenses; used by runtime/caches.

## 5. Notices

- Generate `THIRD_PARTY_NOTICES.txt` from [`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md) + the dependency map ([dependencies.md](../specs/dependencies.md)).
- Include Apache-2.0 NOTICE, MIT/BSD copyright, OFL/FTL, ICU/Unicode notices.
- Expose an About/licenses UI in the app.
- CI checks notices cover the link graph and fail on new unlisted dependencies.

### Shipping license compliance

A distributable must contain three files. `cmake/tessera_packaging.cmake` installs them under
`${CMAKE_INSTALL_DATADIR}/tessera/licenses` (normally `share/tessera/licenses`) and fails the
configure step loudly if any is missing:

| File | Contents |
|---|---|
| `LICENSE` | the project MIT license |
| `THIRD_PARTY_NOTICES.md` | component map: files, upstream, copyright |
| `THIRD_PARTY_LICENSES.txt` | generated bundle: component -> license -> copyright map, then each distinct license text once |

The canonical texts live in [`licenses/`](../../licenses/README.md), one file per license with a
provenance header. Regenerate and verify the bundle from the repository root:

```bash
python -B scripts/gen_license_bundle.py --write
python -B scripts/gen_license_bundle.py --check
```

`--check` fails on a stale bundle, a production-linked license without a local text, or a
component without a resolved copyright holder; the cheap `release-gates` CI job runs it. Regenerate
the bundle in the same change as any dependency/notices update and ship the result with the app.
The About/licenses UI should show these files. The bundle covers third-party code linked into the
binary; system libraries that stay dynamic (Vulkan loader/ICDs, OS frameworks, glibc) are not
linked third-party code and are not part of the bundle.

## 6. Windows

- Static CRT (`/MT`), x64.
- Ship `.exe` + assets (or embedded resources).
- Code signing is optional here; the policy is an open question (section 12) and [updates-and-distribution-v1](../specs/updates-and-distribution-v1.md) states when it is mandatory; ship a version resource/manifest.
- Test on a clean VM with GPU drivers but no Vulkan SDK.
- Archive PDBs separately; do not ship debug binaries.

## 7. Linux

- Ship executable + assets; glibc/system libs dynamic.
- If a fully static binary is required, evaluate musl (an alternative C library; no decision yet, see section 12); GPU drivers remain dynamic.
- AppImage/Flatpak/tarball: document sandbox/GPU access and Vulkan ICD discovery for each format.
- Strip symbols from the shipped binary; ship debug symbols as a separate artifact.
- Verify the lavapipe (Mesa software Vulkan driver) fallback and a debug build with no validation-layer errors.

## 8. Versioning

- SemVer for the framework and generated projects (see [`versioning-v1.md`](../specs/versioning-v1.md)).
- `tessera.json` schema version, IR version, bindings manifest version, golden report version.
- Changing defaults/meaning requires a schema bump and `tessera migrate`.
- Record the exact toolchain/dependency versions in the release artifact.

## 9. Reproducibility

- Pin dependencies, shader compiler (`glslc`), lavapipe, fonts.
- Avoid timestamps/absolute paths in generated code (required by [code-style.md](code-style.md)) and in binaries (use the prefix-map flags below).
- Use `-ffile-prefix-map`/`-fdebug-prefix-map`; no `__DATE__`/`__TIME__`.
- Produce an SBOM and a dependency lock.
- CI builds from a clean checkout.

## 10. Release checklist

- [ ] SemVer/changelog updated
- [ ] Windows + Linux clean builds/tests
- [ ] validation layers clean (debug)
- [ ] link-graph allowlist passes
- [ ] no JS/Wasm/React runtime or dynamic plugin loader in the production binary ([ADR-024](../../decisions/ADR-024-native-first-boundaries.md), native-first boundaries)
- [ ] bridge contract/leak tests pass (bridges: [docs/design/bridges.md](../design/bridges.md))
- [ ] notices/SBOM generated and shipped
- [ ] assets/fonts/ICU data packaged
- [ ] clean-VM smoke test (no SDK)
- [ ] crash symbols archived
- [ ] golden baselines (reference images) reviewed if they changed
- [ ] docs/ADRs updated

## 11. Rollback

- Keep previous artifacts and notices.
- If a release is bad, publish a fixed version; do not silently replace binaries.
- Rotate any secrets exposed by logs/artifacts.
- Document the incident and add a regression test.

## 12. Open questions

Answered elsewhere (not open): code signing/notarization, installer vs portable choice (documented per app), and update mechanism/signature verification are defined in [updates-and-distribution-v1](../specs/updates-and-distribution-v1.md).

Still open (no decision yet):

- SBOM format/tooling.
- Fully static Linux (musl) support.
