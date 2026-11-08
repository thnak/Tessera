# Versioning and compatibility v1

Status: proposed. Companion to the per-artifact specs listed in section 2. Applies to the framework, generated code, project config, IR, manifests, and
golden reports.

Terms: **IR** is the compiler intermediate representation ([`ir-v1.md`](ir-v1.md)). **ABI** is
binary interface compatibility. **SBOM** is a software bill of materials. **CRT** is the C runtime
library. **LTS** is long-term support. **Golden** is a deterministic reference image
([`golden-tests-v1.md`](golden-tests-v1.md)). **Protected region** is developer C++ preserved across
regeneration ([glossary](../reference/glossary.md)). **V1** is the first release scope
([`AGENTS.md`](../../AGENTS.md#non-negotiables), item 8).

## 1. Framework version

- Semantic Versioning: `MAJOR.MINOR.PATCH`.
- Pre-1.0: minor versions may break APIs; document migrations.
- 1.0+: breaking API/behavior changes require a major version.
- Generated projects record the Tessera version used.

## 2. Schema versions

| Artifact | Version field | Current | Compatibility |
|---|---|---|---|
| `tessera.json` | `schemaVersion` | 1 | strict; unknown fields error |
| IR | `irVersion` | 1 | codegen rejects unknown |
| Bindings manifest | `manifestVersion` | 1 | merger/generator must match |
| Golden case/report | `caseVersion`/`reportVersion` | 1 | runner rejects unknown |
| Shader manifest | `manifestVersion` | 1 | build rejects mismatch |
| Asset manifest | `manifestVersion` | 1 | runtime validates |
| Restore snapshot | `restoreVersion` | 1 | forward migration; drop incompatible entries |
| Update manifest | `manifestVersion` | 1 | signed; platform/arch/channel checked |
| i18n catalog | `catalogVersion` | 1 | compiled; missing locale falls back |
| Devtools protocol | `protocolVersion` | 1 | unknown major rejected |
| Crash report | `reportVersion` | 1 | grouping/symbol metadata compatible |
| Design tokens | `tokenVersion` | 1 | SemVer; visual changes need goldens |

Defining specs: `tessera.json` in [`project-config-v1.md`](project-config-v1.md); IR in
[`ir-v1.md`](ir-v1.md); golden case/report in [`golden-tests-v1.md`](golden-tests-v1.md); shader
manifest in [`shader-manifest-v1.md`](shader-manifest-v1.md); restore snapshot in
[`state-restoration-v1.md`](state-restoration-v1.md); update manifest in
[`updates-and-distribution-v1.md`](updates-and-distribution-v1.md); i18n catalog in
[`i18n-v1.md`](i18n-v1.md); devtools protocol in
[`devtools-protocol-v1.md`](devtools-protocol-v1.md); crash report in
[`crash-reporting-v1.md`](crash-reporting-v1.md); asset manifest in
[`resources.md`](../design/resources.md). The bindings manifest and design tokens have no
dedicated spec in this folder.

Rules:

- Adding optional fields keeps the version.
- Changing defaults/meaning or removing fields bumps the version.
- Bumps ship a migration command/tool and a compatibility note.
- Old artifacts fail loudly with a clear error; never silently reinterpret.
- Restore/update/catalog/crash artifacts carry the app version/build id (restore snapshot:
  [`state-restoration-v1.md`](state-restoration-v1.md); update manifest:
  [`updates-and-distribution-v1.md`](updates-and-distribution-v1.md)); a
  mismatch falls back safely, never executes untrusted content.
- Devtools/crash/restore schemas are not part of the framework ABI; they are versioned wire/
  storage formats.

## 3. Generated code

- Generated C++ is tied to the Tessera version that produced it.
- Regeneration is deterministic; `--check` detects drift.
- Protected regions are a compatibility contract: marker format/id semantics cannot change
  without a migration.
- Generated code targets the project's declared C++ standard (C++20 in V1).

## 4. Runtime ABI

- Static linking means no stable binary plugin ABI in V1.
- Public C++ headers are intended to be source-compatible within a minor version; no stricter
  guarantee or exception list is specified.
- Internal headers (`src/`, `detail/`) are not stable.
- Prebuilt third-party libraries must match the project's CRT/ABI.

## 5. Configuration migrations

```bash
tessera migrate --to <schemaVersion>
```

- Migrations are deterministic and idempotent.
- They rewrite `tessera.json` and generated manifests, not developer C++.
- A migration report lists changed fields/defaults.
- Unknown future fields are rejected, not ignored.

## 6. Deprecation

- Mark deprecated fields/APIs in docs and diagnostics.
- Keep them for at least one minor release (pre-1.0 may be shorter; the shorter period is stated
  in the migration note).
- Removal requires a version bump and migration note.
- Runtime warnings include the replacement.

## 7. Compatibility matrix

Document per release (the compatibility matrix itself is not written yet):

- supported OS/architectures
- Vulkan 1.3 / WebGPU feature floor
- compiler/toolchain versions
- C++ standard
- dependency versions (SBOM)
- schema/IR versions

## 8. Golden/report compatibility

- Baselines record the renderer/compiler version.
- Changing rounding, AA, color space, or font data requires baseline review.
- Report schema changes bump `reportVersion`; consumers tolerate unknown optional fields only
  when documented ([`golden-tests-v1.md`](golden-tests-v1.md) section 10 covers report
  versioning but does not list which fields are optional).

## 9. Release notes

Include:

- breaking changes and migrations
- schema/IR/manifest versions
- dependency/license changes
- known issues/fallbacks
- golden baseline changes (with links)

## 10. Open questions

- Whether to adopt CalVer for the framework.
- Long-term support (LTS) policy.
- ABI stability for future dynamic modules (currently out of scope).
- SBOM/version metadata embedding in binaries.
