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

A distributable must contain four files. `cmake/tessera_packaging.cmake` installs them under
`${CMAKE_INSTALL_DATADIR}/tessera/licenses` (normally `share/tessera/licenses`) and fails the
configure step loudly if any is missing:

| File | Contents |
|---|---|
| `LICENSE` | the project MIT license |
| `THIRD_PARTY_NOTICES.md` | component map: files, upstream, copyright |
| `THIRD_PARTY_LICENSES.txt` | generated bundle: component -> license -> copyright map, then each distinct license text once |
| `sbom.spdx.json` | generated SPDX 2.3 JSON SBOM for production-linked components (see below) |

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

### Software bill of materials

`sbom.spdx.json` is an SPDX 2.3 JSON document describing the project package and every
production-linked third-party component (the same rows as the license bundle; test, CI, build-time
and design-time rows are excluded). Each package records its license expression
(`licenseConcluded`/`licenseDeclared`), copyright, `downloadLocation` where the notices file has an
upstream URL, a pinned `versionInfo` where the dependency map pins one, and `filesAnalyzed: false`.
Licenses without an SPDX identifier use an `LicenseRef-*` with an `hasExtractedLicensingInfos`
entry (currently SQLite's public-domain dedication).

The document is byte-deterministic: the creation time and tool creator are fixed, the
`documentNamespace` is content-derived and packages/relationships are sorted, so two runs and two
checkouts produce identical bytes. Regenerate and verify from the repository root:

```bash
python -B scripts/gen_sbom.py --write
python -B scripts/gen_sbom.py --check
```

`--check` fails on drift and is part of the `release-gates` CI job. The SBOM installs next to the
license files under `share/tessera/licenses`. Out of scope: build-time/CI tooling and deep
transitive build dependencies, per-file checksums or provenance attestations
(`filesAnalyzed: false` means no file hashes), and vulnerability/SCA data.

### Distributable archive

CPack can produce a release archive, gated by `scripts/check_package.py`. Packaging is opt-in, so a
normal configure and `cmake --install` are unchanged; pass the explicit executable set:

```bash
cmake -S . -B build/package \
  -DCMAKE_BUILD_TYPE=Release -DTESSERA_STATIC_RUNTIME=ON \
  -DTESSERA_BUILD_VULKAN=OFF -DTESSERA_BUILD_TESTS=ON \
  -DTESSERA_PACKAGE_EXECUTABLES=static_runtime_probe
cmake --build build/package --config Release --target tessera-package-stage
export SOURCE_DATE_EPOCH=315532800 TZ=UTC
cpack --config build/package/CPackConfig.cmake -G TGZ -B build/package/out
python -B scripts/check_package.py --archive build/package/out/tessera-*.tar.gz
```

`SOURCE_DATE_EPOCH` (1980-01-01 UTC by default, the ZIP/DOS minimum) fixes archive entry mtimes and
`TZ=UTC` fixes ZIP DOS timestamps, so two builds of the same staged content produce the same
archive bytes; CI package steps set both and the reproducible proof asserts byte equality. `python -B scripts/check_reproducible.py
--package` builds twice and requires byte-identical TGZ/ZIP archives, while `check_package.py`
continues to enforce the archive *content*.

The archive (TGZ, plus ZIP on Windows) contains one top-level
`tessera-<version>-<os>-<arch>/` directory with exactly the built executables, `LICENSE`,
`THIRD_PARTY_NOTICES.md`, `THIRD_PARTY_LICENSES.txt`, `sbom.spdx.json`, `PACKAGE-INFO.txt`,
`RELEASE-NOTES.txt` and `SHA256SUMS`. `PACKAGE-INFO.txt` and `RELEASE-NOTES.txt` record the
version, commit, platform and static-runtime statement; `SHA256SUMS` lists every shipped file except
itself (sorted `sha256  file` lines, LF). CPack packages a staged directory rather than the install
tree, so fetched dependency headers/libraries cannot leak in. `check_package.py` verifies the
layout and executable set, that the SBOM parses as SPDX-2.3, that the license bundle covers every
production-linked license, that the static runtime is ON, that the checksums match every file, that
`RELEASE-NOTES.txt` repeats the metadata and verification commands, and that `PACKAGE-INFO.txt`
leaks no build paths or timestamps; `--json` gives a machine-readable report.

Verify a downloaded archive (one command per OS, run inside the unpacked directory):

```bash
sha256sum -c SHA256SUMS                                  # Linux/macOS
certutil -hashfile <file> SHA256                         # Windows (cmd)
Get-FileHash -Algorithm SHA256 <file>                    # Windows (PowerShell)
```

What it proves: the shipped artifact links the static C/C++ runtime and carries complete
license/SBOM material next to the binaries. What it does not prove: installer signing, per-OS
installer formats (MSIX/dmg/AppImage), translated assets, or a clean-VM smoke run. CI builds,
packages and checks the archive without uploading it: the checker fixtures run on every PR and the
archive itself is a release-time artifact produced with the command above.

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
- The update host and the signing key (section 15 records the empty state; filling either is an
  owner decision, not a tooling one).

## 13. Run the gates locally

`scripts/gates.py` runs the fast repository gates in a fixed order, streams each gate's output and
prints one summary table:

```bash
python -B scripts/gates.py
```

Gate order: `format-version` (the `clang-format` on `PATH`/`CLANG_FORMAT` matches the version
pinned in `tools/format-version.txt`, which is also what CI installs), `dependencies` (license
allowlist, statuses, notices), `license-bundle` (`THIRD_PARTY_LICENSES.txt` is current), `sbom`
(`sbom.spdx.json` is current), `docs` (links/anchors/index/fences/sizes/ADRs), `source-size` (the
500-line ratchet), `public-api` (the recorded public surface, section 13.1), `pins` (reproducible
fetch pins plus dependency rows), `version-policy` (a breaking change to the RHI headers, the IR
schema or `tessera.json` moves that surface's version; policy in
[versioning-v1](../specs/versioning-v1.md) section 11, baseline in
`scripts/versioning-baseline.txt`, section 13.2), `memory-budget` and `release-manifest` (the channel
file, the release ledger and the projected update manifests, section 15.3).
`--archive PATH` adds `package` (validates a distributable archive) and `--fixtures` adds the slower
gate self-tests. `--only docs,pins`, `--list`, `--json` and `--quiet` are available, and the runner
is cwd-independent; it forces UTF-8 for child processes so it behaves the same on Windows cp1252
consoles as on Linux/macOS.

### 13.1 The public-API surface gate

`scripts/check_public_api.py` keeps `scripts/public-api-baseline.txt`, the committed record of the
public surface (issue #76). It exists because nothing recorded the surface, so a header deleted, a
type renamed, a prop dropped or a `::part()` name removed could land with nobody noticing. A diff of
the baseline **is** the API diff: an added line is additive, a removed or renamed line is breaking
([`versioning-v1.md`](../specs/versioning-v1.md) section 1).

The baseline records, one sorted line each (`kind path name`):

- every `include/tessera/**/*.hpp` as a `header` line, plus the `class`/`struct`/`union`/`enum`,
  `alias`, `concept`, `function`, `constant` and `variable` declared at namespace scope in it, fully
  qualified (`class include/tessera/ui/scene/node.hpp tessera::ui::Node`);
- every `packages/tessera-ui/**/*.tsx` as a `component` line, the fields of its
  `export type <Name>Props` as `prop` lines and its literal `part="a b"` names as `part` lines (the
  `::part()` contract of ADR-047 item 3, the same source `tessera_codegen` builds the library part
  registry from). No library component declares a `part="..."` yet, so today the baseline has
  `component` and `prop` lines from it and no `part` lines; the first component that adds one starts
  being tracked, and dropping it later fails the gate.

It is **declaration-level only** and deliberately does not record: function signatures, return
types, template parameters, attributes or access specifiers; class members (methods, data members,
nested types, enumerators); default values, prop types or prop optionality; `::part()` names used in
stylesheets; or the `tessera.json` schema (issue #76 leaves config schemas to #194). Recording those
would make every ordinary edit a reported "break", and a gate that fires on routine work gets
switched off, which is worse than no gate. What it does catch is surface *disappearing*, which is
never routine.

```bash
python -B scripts/check_public_api.py --check    # the gate (scripts/gates.py runs this)
python -B scripts/check_public_api.py --update    # record additions
```

`--update` is a ratchet, like `scripts/check_source_size.py`: it records additive lines and **refuses
to write anything when a line would be dropped**, so removing or renaming surface is a hand edit of
the baseline in a reviewed change, with the reason in the diff. The deleted line is the record of
the break. Exit codes: 0 match, 1 a difference or a refused `--update`, 2 a missing input.

### 13.2 The version-policy gate

`scripts/check_version_policy.py` is the other half (issue #194, policy in
[`versioning-v1.md`](../specs/versioning-v1.md) section 11). The public-API gate answers *did the
surface change?*; this one answers *did that change require a version bump?* They have separate
baselines and neither reads the other.

It watches three surfaces, each with the version its spec already declares:

| Surface | Version read from |
|---|---|
| RHI, `include/tessera/rendering/rhi/**` | `kRhiVersionMajor`/`kRhiVersionMinor` in `.../rhi/version.hpp` |
| IR schema, [`ir-v1.md`](../specs/ir-v1.md) + [`ir-v1-expr.md`](../specs/ir-v1-expr.md) | `kIrVersion` in `.../ir/module.hpp` |
| `tessera.json`, incl. its `runtime` and `diagnostics` objects | the accepted `schemaVersion` in `src/harness/manifest_loader.cpp` |

`scripts/versioning-baseline.txt` records the reviewed state of all three. A **breaking** change
(removed or retyped field, a field made required, a changed default) that leaves the version where
it was fails the gate and names every field; a change to the RHI headers that leaves the version
alone fails too. An **additive** change keeps the version but is still recorded:

```bash
python -B scripts/check_version_policy.py --check     # the gate (scripts/gates.py runs this)
python -B scripts/check_version_policy.py --update    # record a reviewed change
```

`--update` refuses a breaking change whose version did not move, so it records a decision and never
hides one. A version bump and its recording ship in the same change. Exit codes: 0 clean, 1 a
finding or a refused `--update`, 2 an unreadable surface input.

OS matrix: the same `gates.py` runs in the `release-gates` job on Linux, Windows and macOS CI. The
gate self-test suite (`--fixtures`) stays on Linux, where it is cheap; the Windows/macOS jobs run
only the aggregator so they stay around a minute. The release-only checks (static runtime,
package contents, reproducible build) run in the platform build jobs on Linux and Windows.

## 14. Release checklist

Run the steps below from a clean checkout of the release commit on each OS; do not publish until all
three platforms are green. Steps 1-2 run everywhere; step 5 produces one artifact per OS.

### Clean-room rehearsal

Before the numbered steps, run the clean-room rehearsal, which builds from the exact commit in a
detached temporary worktree, so a build input that exists only in a dirty working tree fails
instead of leaking into a release:

```bash
python -B scripts/release_rehearsal.py --target static_runtime_probe
```

It resolves `git rev-parse HEAD`, configures Release with `TESSERA_STATIC_RUNTIME=ON` and
`TESSERA_PACKAGE_EXECUTABLES=<target>`, builds the target set, creates the TGZ/ZIP archive, and runs
in order `check_package.py`, the `cmake --install` license/SBOM layout check, `check_static_link.py`,
`gen_sbom.py --check`, `gen_license_bundle.py --check` and `check_dependencies.py`, then prints one
summary table and removes its worktree (`--keep` keeps it for inspection; `--skip-build --artifact
PATH` reruns only the gate steps). Expected duration is roughly 1-2 minutes on Linux CI and up to
~5 minutes for a first Windows run (network FetchContent plus the MSVC environment). It complements,
and does not replace, `gates.py` (current-tree checks including docs) and the per-OS CI
build/package jobs; it runs in the Linux `release-rehearsal` CI job.

1. **Repository gates** — `python -B scripts/gates.py` (format-version, dependencies, license
   bundle, SBOM, docs, source-size, public-api, pins, version-policy, memory-budget,
   release-manifest). Proves the tree is internally consistent; add `--archive` once step 5 produced
   an archive.
2. **Release build and static-runtime proof** — configure with `TESSERA_STATIC_RUNTIME=ON` and
   `BUILD_SHARED_LIBS=OFF` (both default), build the shipped target set, then run
   `python -B scripts/check_static_runtime.py --config Release --target <target>`. Proves the
   C/C++ runtime is linked statically (ADR-012).
3. **Install layout** — `cmake --install <build> --prefix <temp> --component licenses`; the prefix
   must contain `share/tessera/licenses/{LICENSE,THIRD_PARTY_NOTICES.md,THIRD_PARTY_LICENSES.txt,
   sbom.spdx.json}`. Proves the installer ships the compliance files even outside the archive.
4. **Reproducible proof** — `python -B scripts/check_reproducible.py --config Release --target
   <target> --package`. Builds twice and requires both the executable and the CPack archive
   (`SOURCE_DATE_EPOCH=315532800`, `TZ=UTC`) to be byte-identical (Linux/Windows CI; macOS when its
   toolchain supports the same flags). `--commit` defaults to `git rev-parse HEAD`; that exact
   commit is checked out in a detached worktree for both builds and written to
   `PACKAGE-INFO.txt`/`RELEASE-NOTES.txt`, so the metadata cannot drift from the built sources and
   a moving HEAD or dirty working tree cannot skew the comparison.
5. **Distributable archive** — configure with `-DTESSERA_PACKAGE_EXECUTABLES=<targets>`, build
   `tessera-package-stage`, run `cpack --config <build>/CPackConfig.cmake -G <TGZ|ZIP> -B <out>`,
   then `python -B scripts/check_package.py --archive <artifact>`. Proves the archive contains
   exactly the shipped executables plus the compliance files, a valid SPDX-2.3 SBOM, a sorted
   `SHA256SUMS` covering every file, and a `RELEASE-NOTES.txt` with the metadata and verification
   commands, with no build-path or timestamp leaks. Users verify with `sha256sum -c SHA256SUMS`
   (Linux/macOS), `certutil -hashfile <file> SHA256` or `Get-FileHash -Algorithm SHA256 <file>`
   (Windows). Artifact names:
   - Linux: `tessera-<version>-linux-x86_64.tar.gz`
   - Windows: `tessera-<version>-windows-x86_64.zip` (a `.tar.gz` is generated as well)
   - macOS: `tessera-<version>-macos-arm64.tar.gz` on Apple Silicon (the `macos-latest` runner;
     `tessera-<version>-macos-x86_64.tar.gz` on Intel runners)
6. **Static-link gate on the artifact** — `python -B scripts/check_static_link.py --os <os>
   --binary <exe>` (the platform build jobs do this); proves no unexpected dynamic third-party
   library and no dynamic MSVC/`libstdc++`/`libgcc_s` runtime.
7. **Review and publish (manual)** — code signing and notarization, installer formats
   (MSIX/dmg/pkg/AppImage), a clean-VM smoke test without the Vulkan SDK, artifact upload and
   release notes stay manual. No signing or publishing automation is provided.

The platform CI jobs run steps 1-2 and 3-6 on Linux, Windows and macOS; step 7 is always manual.

### Release CI lanes: validation layers, goldens and artifacts

Issue #199's acceptance is "release builds on all lanes run validation-layer and golden suites and
publish artifacts". There is no separate `release.yml`: the release profile is the existing gating
lanes (`.github/workflows/linux.yml`, `windows.yml`), and the `release-rehearsal` and
`static-runtime-proof` jobs above cover the *artifact* half of that sentence only — the rehearsal
builds and validates the archive but runs no `ctest`, and both configure with
`TESSERA_BUILD_VULKAN=OFF`, so neither exercises a GPU. The runtime half runs in the build jobs.

**Validation layers cost a package, not a build option.** The layer is chosen at runtime by
`rhi::DeviceDesc::validation` and installed by `VulkanDevice`
(`src/rendering/backend/vulkan/src/vulkan_instance.cpp`), which enables `VK_LAYER_KHRONOS_validation`
with `VK_EXT_validation_features` synchronization validation. There is no `TESSERA_VULKAN_VALIDATION`
option, so a Release build runs the layer exactly like a Debug build. The `linux` job installs
`vulkan-validationlayers` and asserts the layer manifest is present.

**The matrix and the two-mode gate.** `scripts/check_release_suites.py` owns `LANE_MATRIX`, the
single source for which suite runs on which lane. Every release-lane job declares its list twice, on
purpose: in the workflow (readable, reviewable, so the lane states what it claims) and in the matrix
(enforced). The `release-gates` job runs the script with `--workflows`, which compares the two and
fails in **both** directions — a suite the lane stopped requiring, an `--allow-skip` that lost its
reason, and also a workflow name the matrix does not know. That static half is what catches a lane
silently dropping a suite, because a runtime report can only omit what it was never asked for.

The runtime half (`--lane`, from the `--output-junit` report plus the CTest log) additionally proves
the declared suites actually *ran*, which the exit code cannot: `batched2d_vulkan_validation` only
prints `[vulkan] warning: validation layer not available` and then **passes**; the gallery and
CPU-vector goldens carry `SKIP_REGULAR_EXPRESSION`; and CTest exits 0 when the filter matched
nothing. Two details the gate has to get right: `ctest --preset` resolves a **relative**
`--output-junit` path against the preset's test directory (so `build/ci/x.xml` lands in
`build/ci/build/ci/x.xml`, hence the absolute `$PWD/...` in both workflows), and a Windows
`Tee-Object` log is UTF-16LE, which the gate detects rather than reading as UTF-8.

What each lane runs, and what it cannot:

| Suite | `linux` | `windows` | Why Windows cannot |
|---|---|---|---|
| `batched2d_vulkan_validation`, `batched2d_on_demand`, `vulkan_*`, `glass_*_gpu`, `msaa`, `rounded_clip`, `renderer_seam_gpu`, `paint_*_gpu` | run | cannot | the job configures `TESSERA_BUILD_VULKAN=OFF`; there is no Vulkan instance |
| `batched2d_golden`, `batched2d_overlay_golden`, `batched2d_text_golden`, `batched2d_shapes_golden`, `gallery_golden_*`, `harness_frame_capture` | run | cannot | lavapipe-only; the entries are registered under `if (UNIX AND TARGET batched2d_poc)` and a hosted Windows runner has no lavapipe |
| `batched2d_cpu_image_golden`, `cpu_wide_golden`, `theme_golden`, `harness_golden`, `lowering_golden`, `schema_golden` | run | run | GPU-free and bit-exact on both hosts |
| `batched2d_cpu_golden`, `batched2d_cpu_text_golden` | run | cannot | need `batched2d_poc`, which exists only with the Vulkan backend on |
| `batched2d_cpu_vector_golden`, `codegen_vector_paint_golden` | run | cannot | gated on `TESSERA_ENABLE_THORVG`, which this lane leaves off |

The gate also reports a **stale exemption**: if a suite declared unable to run does run (for example
`batched2d_vulkan_validation` on a developer's GPU box), the log says "drop the exemption", so the
matrix cannot quietly keep excusing a gap that closed.

So "all lanes" means validation layers and the lavapipe/GPU goldens on Linux, and the GPU-free
bit-exact golden corpus on Windows. Claiming the lavapipe goldens on Windows would be false. The
`release-artifacts-linux` / `release-artifacts-windows` artifacts carry every golden PNG (frame,
diff, candidate baseline), the JUnit report and the CTest log, kept 30 days. The distributable
archive is still *not* uploaded (§5): it stays a release-time artifact.

Two deliberate non-additions: enabling ThorVG on Windows would add a network fetch and 37 sources to
a job whose 50-minute cap is already tight (#258), and the WebGPU pixel comparison still needs the
owner decision recorded in `docs/design/compiler-wasm.md` §8. No new dependency, runner or service
is introduced — validation layers and lavapipe were already installed and are already in
[`dependencies.md`](../specs/dependencies.md).

## 15. Updates and distribution: the manifest, the channels and the ledger

Issue #193. Normative rules: [`updates-and-distribution-v1.md`](../specs/updates-and-distribution-v1.md)
sections 1-3 and 6.1-6.3. This section is the how-to and the rationale; the spec wins on any conflict.

The archive of section 5 is a file. An *update* additionally needs four answers: which artifacts
exist, what their digests are, which channel a user may be offered them on, and whether a client is
allowed to apply one. Those are three committed documents:

| Document | Answers | Written by |
|---|---|---|
| `release/channels.json` | who may move between channels, consent, host allowlist, pinned keys | a reviewed edit |
| `release/ledger.json` | what shipped: version, channel, date, commit, archive and symbol digests | `gen_release_manifest.py record` |
| `release/manifests/<channel>-<platform>.json` | the specification section-2 manifest a client reads | `gen_release_manifest.py project` |

### 15.1 Recording a release

`record` takes the bytes, not a claim: it hashes the archive and its symbol artifact, reads the
version/platform from the archive name (the contract `check_package.py` enforces), the commit from
`PACKAGE-INFO.txt`, and the build id from the symbol file's header. It refuses a `--commit` that
disagrees with `PACKAGE-INFO.txt`, a symbol file for another build, a symbol file with no `func=`
rows, `StaticRuntime` other than `ON`, and a version already recorded on that channel. The ledger is
append-only; a correction is a new version, not an edit.

The **symbol artifact** is `tessera-<version>-<platform>.symbols.txt` in the format
`include/tessera/diagnostics/crash_symbols.hpp` defines. Issue #192 measured that a Release Windows
build emits no PDB; the `/MAP` the linker can produce is one link option, and `read_linker_map_symbols`
already turns it into this artifact. A release that ships none is a gate failure, because a crash
report from it would carry addresses and no names.

```bash
python -B scripts/gen_release_manifest.py record \
  --archive build/package/out/tessera-1.0.0-windows-x86_64.zip \
  --symbols build/package/out/tessera-1.0.0-windows-x86_64.symbols.txt \
  --channel beta --date 2026-11-16
```

### 15.2 Projecting and checking

```bash
python -B scripts/gen_release_manifest.py project          # write what changed
python -B scripts/gen_release_manifest.py project --check  # report drift, write nothing
python -B scripts/check_release_manifest.py                # the gate (gates.py: release-manifest)
```

The projection is a ratchet in both directions. `project` refuses to write when the data breaks a
rule (so a bad host, or a version newer than `project(Tessera VERSION ...)`, cannot be committed as a
manifest), and `check_release_manifest.py` fails when the committed bytes differ from the projection.
One side of a metadata change cannot land alone.

What it proves, and what it does not: the manifests describe the exact bytes a ledger recorded, on
channels whose transition rules are explicit, with the deprecation window (two minors **and** ninety
days, [versioning-v1](../specs/versioning-v1.md) section 11.5) computed into each deprecation's
`earliestRemoval`. It does **not** prove a signature, fetch a URL, or apply anything.

### 15.3 Why the client is only a seam

`scripts/release_client.py` decides `offer` / `up-to-date` / `blocked` from the two data files, names
the rule that refused, carries the section-6.1 state machine as data, and encodes G-23's
two-failed-launch rollback and the "no update while unsaved work is active" rule.
`UpdateClientPolicy` is the interface a real client implements; its `check`/`download`/`stage`/`apply`
raise `NotImplementedError`, because each needs a transport, a filesystem slot or an installer, and no
test in this repository may open a socket or read the clock.

**Signing is not implemented and cannot be here.** It needs an Ed25519 private key held by a release
engineer and a CI secret. `release/channels.json` therefore pins no key (the honest unsigned state,
reported as a warning), every manifest's `signature` is `null`, and `plan_update` refuses to offer an
unsigned package: `signature_verified` is an input the caller's key store supplies, never a claim this
tooling makes. Not landed either, with the reason each needs: the CDN and update root (a host and a
budget decision, so `updateHosts` is empty), the downloader with resume and partial-file recovery (a
transport), staging/atomic swap/rollback-apply (a filesystem slot and an installer), and delta
packages (G-23 defers them to v1.1).
