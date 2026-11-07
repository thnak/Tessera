# Versioning and compatibility v1

Status: proposed. Applies to the framework, generated code, project config, IR, manifests, and
golden reports.

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

Rules:

- Adding optional fields keeps the version.
- Changing defaults/meaning or removing fields bumps the version.
- Bumps ship a migration command/tool and a compatibility note.
- Old artifacts fail loudly with a clear error; never silently reinterpret.

## 3. Generated code

- Generated C++ is tied to the Tessera version that produced it.
- Regeneration is deterministic; `--check` detects drift.
- Protected regions are a compatibility contract: marker format/id semantics cannot change
  without a migration.
- Generated code targets the project's declared C++ standard (C++20 in V1).

## 4. Runtime ABI

- Static linking means no stable binary plugin ABI in V1.
- Public C++ headers are source-compatible within a minor version where practical.
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
- Keep them for at least one minor release (pre-1.0 may be shorter, documented).
- Removal requires a version bump and migration note.
- Runtime warnings include the replacement.

## 7. Compatibility matrix

Document per release:

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
  when documented.

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
