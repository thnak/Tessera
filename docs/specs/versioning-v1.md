# Versioning and compatibility v1

Status: accepted for V1 (2026-11-16; sections 1-10 were proposed, section 11 is the policy this
status covers). Companion to the per-artifact specs listed in section 2. Applies to the framework,
generated code, project config, IR, manifests, and golden reports.

Terms: **IR** is the compiler intermediate representation ([`ir-v1.md`](ir-v1.md)). **ABI** is
binary interface compatibility. **SBOM** is a software bill of materials. **CRT** is the C runtime
library. **LTS** is long-term support. **Golden** is a deterministic reference image
([`golden-tests-v1.md`](golden-tests-v1.md)). **Generated base** and **developer class** are the two
halves of a component's C++ under ADR-040 ([glossary](../reference/glossary.md)). **V1** is the first
release scope
([`AGENTS.md`](../../AGENTS.md#non-negotiables), item 8). **Surface** is a versioned artifact whose
shape other people depend on (section 11.1). **Breaking change** is a change to a surface that a
correct consumer of the previous shape would have to change code or data for.

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
| Bindings manifest | `manifestVersion` | 1 | codegen and generator must match |
| Golden case/report | `caseVersion`/`reportVersion` | 1 | runner rejects unknown |
| Shader manifest | `manifestVersion` | 1 | build rejects mismatch |
| Asset manifest | `manifestVersion` | 1 | runtime validates |
| Restore snapshot | `restoreVersion` | 1 | forward migration; drop incompatible entries |
| Update manifest | `manifestVersion` | 1 | signed; platform/arch/channel checked |
| i18n catalog | `catalogVersion` | 1 | compiled; missing locale falls back |
| Devtools protocol | `protocolVersion` | 1 | unknown major rejected |
| Crash report | `reportVersion` | 1 | grouping/symbol metadata compatible |
| Design tokens | `tokenVersion` | 1 | SemVer; visual changes need goldens |
| Compiler diagnostics document | `diagnosticsVersion` | 1 | strict; a consumer rejects an unknown version |

Defining specs: `tessera.json` in [`project-config-v1.md`](project-config-v1.md); IR in
[`ir-v1.md`](ir-v1.md); golden case/report in [`golden-tests-v1.md`](golden-tests-v1.md); shader
manifest in [`shader-manifest-v1.md`](shader-manifest-v1.md); restore snapshot in
[`state-restoration-v1.md`](state-restoration-v1.md); update manifest in
[`updates-and-distribution-v1.md`](updates-and-distribution-v1.md); i18n catalog in
[`i18n-v1.md`](i18n-v1.md); devtools protocol in
[`devtools-protocol-v1.md`](devtools-protocol-v1.md); crash report in
[`crash-reporting-v1.md`](crash-reporting-v1.md); asset manifest in
[`resources.md`](../design/resources.md); compiler diagnostics document in
[`compiler-diagnostics-v1.md`](compiler-diagnostics-v1.md). The bindings manifest and design tokens
have no dedicated spec in this folder.

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
- Regeneration is deterministic; `--check` detects drift of the generated base.
- The generated base is build output and has no compatibility contract; the developer class is
  version-controlled and never rewritten by the generator, so binding-id semantics are the only
  stable interface ([ADR-040](../../decisions/ADR-040-generated-base-and-developer-class.md)).
- Generated code targets the project's declared C++ standard (C++23 in V1).

## 4. Runtime ABI

- Static linking means no stable binary plugin ABI in V1.
- Public C++ headers are source-compatible within a minor version; a breaking source change is a
  major bump (section 11.2). There is no exception list: a header under `include/tessera/**` is
  public unless it is marked otherwise in its own comment.
- The **RHI** is the one surface with a separately declared version
  (`kRhiVersionMajor`/`kRhiVersionMinor` in `include/tessera/rendering/rhi/version.hpp`, ADR-049), so
  an out-of-tree backend can name the contract it implements. A breaking RHI change moves the major,
  an additive one the minor, and a change that only touches comments moves neither. Backends declare
  the implemented major with a `static_assert` and report it through `Device::capabilities()`.
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

## 11. Versioning policy

Sections 1-10 say *which* artifacts carry a version. This section is the policy proper: what may
change under which version, who decides, how a deprecation ends, and which change is blocked by CI.
It is the normative answer to issue #194.

### 11.1 Surfaces and who decides

A **surface** is a versioned artifact another person's code or data depends on. Three are gated by
CI (section 11.4); the rest are covered by the rules below and by the per-artifact specs.

| Surface | Version lives in | Owner of a change |
|---|---|---|
| RHI contract, `include/tessera/rendering/rhi/**` | `kRhiVersionMajor`/`kRhiVersionMinor` in `.../rhi/version.hpp` | rendering owner (ADR-049) |
| IR schema, [`ir-v1.md`](ir-v1.md) + [`ir-v1-expr.md`](ir-v1-expr.md) | `kIrVersion` in `.../ir/module.hpp` | compiler owner |
| `tessera.json`, incl. its `runtime` and `diagnostics` objects | accepted `schemaVersion` in the loader | project-config owner (ADR-014) |
| Other schema artifacts (section 2) | their `*Version` field | the owning spec's author |
| Framework API and behaviour | `project(Tessera VERSION ...)` | project owner |

**Decision rule.** The owner of a surface decides whether a change is breaking, and records the
decision in the same change:

- **Patch (`X.Y.Z` → `X.Y.Z+1`).** No surface change at all: bug fixes, performance, docs, tests.
  A patch that changes a surface is a mistake, not a judgement call.
- **Minor (`X.Y` → `X.Y+1`).** Additive surface changes only; behaviour may change where a spec
  says so, never where a consumer could reasonably have depended on the old effect (section 11.3).
- **Major (`X` → `X+1`).** Any breaking change: a removed or renamed field, type, function,
  enumerator or node kind; a narrowed type; a field becoming required; a changed default, meaning,
  error behaviour or documented limit; a new pure virtual member. Pre-1.0 a major bump may happen at
  any time (section 1), but the classification rules still apply, so a breaking change is never
  smuggled into a minor.

**Every schema has exactly one version number.** A nested object in `tessera.json` (`runtime`,
`diagnostics`, `loading`, ...) does not get its own `schemaVersion`; a breaking change to a nested
object moves the document's `schemaVersion`. This is why the gate reads one version per surface.

### 11.2 What counts as breaking, per surface

The general rule is "a correct consumer of the previous shape must change". Concretely:

- **IR**: removing or renaming a field or a node `kind`; changing a field's type; making an optional
  field required; changing what an existing value means; reordering a field whose order is
  significant (`types`, record `members`). Adding an optional field or a new node kind that an
  existing module never emits is **additive** and keeps `irVersion` ([`ir-v1-expr.md`](ir-v1-expr.md)
  section 12).
- **`tessera.json`**: the same list, plus a **changed default** (a project that omitted the field now
  behaves differently) and a **rejected-but-previously-accepted value**. Adding an optional field
  keeps `schemaVersion` ([`project-config-v1.md`](project-config-v1.md) section "Versioning").
- **RHI**: the list in ADR-049 section 2 items 3 and 4 governs. Removing or renaming a public type,
  function or enumerator; changing a signature, a default, an ownership/lifetime rule, an error
  behaviour or a documented threading rule; adding a pure virtual member; changing an enumerator's
  numeric value; tightening a documented limit are **breaking**. Adding a type, a trailing enumerator,
  a trailing defaulted descriptor field, a non-pure virtual with a correct default, or an optional
  capability query are **additive**. A comment-only edit is neither.

### 11.3 Behaviour is part of the surface

A change is breaking when it changes an observable outcome a consumer could have relied on, even
with no signature change: pixel output on the reference backends (section 8), cascade or layout
results, error codes and their conditions, iteration or emission order a spec pins, and the
deterministic bytes of generated files and goldens. Such a change needs at least a minor bump and a
note in the release notes; when the previous outcome was documented as guaranteed, it needs a major.

### 11.4 The gate

`scripts/check_version_policy.py` runs in `scripts/gates.py` (gate `version-policy`) on every PR and
in the `Docs` lane. It records the reviewed state of the three gated surfaces in
`scripts/versioning-baseline.txt` and fails when:

1. a gated surface changed in a **breaking** way and its version did not move — the failure the
   policy exists for;
2. a gated surface changed at all and the baseline was not re-recorded with `--update` in the same
   change, so a version bump and its baseline can never be split across two commits;
3. a version moved **backwards** or, for RHI, did not increase its major/minor.

It passes an additive change that keeps the version and records it. It is deterministic, needs no
build, network or git, and reports sorted `ERROR <path>: <message>` lines naming the surface, the
field, the version site and this section.

**Relationship to the public-API gate (#76).** `scripts/check_public_api.py` records *that* the
public surface changed: it lists the declarations of `include/tessera/**` and the `tessera-ui`
components, props and `::part()` names, and fails when a line disappears. This gate records *whether
that change required a version bump*. They are different mechanisms with separate baselines (#76 owns
`scripts/public-api-baseline.txt`, #194 owns `scripts/versioning-baseline.txt`) and neither reads the
other, so they land and rebase independently. Neither is redundant:

- #76 is **declaration-level by design** and explicitly records no signatures, no class members, no
  enumerators, no defaults and no config schemas. It therefore cannot see an enumerator removal, a
  changed default in a descriptor, or any `tessera.json`/IR field — all of which are breaking under
  section 11.2. It also cannot say whether a change it *does* see was breaking or additive.
- #194 is **version-level**: it sees every edit to the RHI headers (as a digest), every field of the
  IR and `tessera.json` schemas, and it asks the one question #76 cannot: did the version move?

The RHI input here is a digest, not a symbol list, precisely so this gate never re-implements #76's
classifier. #76 answers "what changed", the author's PR states whether that was additive or breaking,
and this gate checks that the version and the recording agree with that statement.

`scripts/check_docs.py` already fails an RHI header change that does not also touch `version.hpp`
and `docs/design/gpu/rhi.md`. That rule answers "which files were edited"; this gate answers "did the
declared version actually move". Both are wanted: the docs rule catches a forgotten companion file,
the policy gate catches a forgotten version.

### 11.5 Deprecation window

- Mark the deprecated entity in its spec, the header and the diagnostics, with the replacement and
  the removal version. In C++ use `[[deprecated("use X; removed in RHI N")]]`.
- Keep it for **at least two minor releases and ninety days**, whichever is longer (ADR-049 item 5),
  counted from the release that first deprecated it. Pre-1.0 the owner may shorten the window; the
  shorter period is then stated in the migration note.
- Removal happens only in a major bump, together with a migration note and a release-note entry
  (section 9).
- The RHI ninety-day floor binds from the first release that promises RHI compatibility to outside
  backends; before that the PR states the impact and the replacement (ADR-049 item 7).

### 11.6 Migrations

A version bump ships a migration unless the bump is a major with no surviving consumers:

- `tessera migrate --to <schemaVersion>` for project config (section 5), deterministic and
  idempotent.
- A migration note in the PR and the release notes: what moved, what the replacement is, what is not
  migrated automatically.
- Old artifacts fail loudly with the version they want and the version they found; they are never
  silently reinterpreted (section 2).

### 11.7 How to make a gated change

1. Classify the change with section 11.2 and state the class in the PR description.
2. Bump the version site for that surface (patch never, minor for additive, major for breaking).
3. Update the surface's spec and, for RHI, `docs/design/gpu/rhi.md`.
4. Run `python -B scripts/check_version_policy.py --update` and commit the updated
   `scripts/versioning-baseline.txt` **in the same change**. `--update` refuses an unversioned
   breaking change, so it records a decision, never hides one.
5. Run `python -B scripts/gates.py`.
