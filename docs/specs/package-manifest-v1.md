# Package manifest `tessera-package.json` v1

Status: V1, library stage 0 ([ADR-047](../../decisions/ADR-047-component-library-layers-and-design-language.md)
item 1, issue #234). Terms such as IR are defined in the [glossary](../reference/glossary.md).

A Tessera package is a directory of ordinary source that an application imports by a bare
specifier (`from "tessera-ui"`, [`jsx-subset-v1.md`](jsx-subset-v1.md) section 4). The application
maps the specifier to the directory in `tessera.json` `packages`
([`project-config-v1.md`](project-config-v1.md)) and may state which version it expects. This file
defines how the package states the version it has and how the compiler compares the two.

## Manifest

`tessera-package.json` sits directly in the package root (`packages[].root`):

```json
{ "name": "tessera-ui", "version": "1.4.0" }
```

| Field | Type | Rule |
|---|---|---|
| `name` | string | Required; equals the `packages` entry's `name` |
| `version` | string | Required; `X.Y.Z`, decimal components without leading zeros; no pre-release or build suffix in V1 |

Unknown fields are ignored so later versions can add metadata (for example a minimum compiler
version) without breaking this check.

The repository's own library package, `packages/tessera-ui/`, carries this manifest (version
`0.1.0`). The bundled copy of that library that the compiler resolves when a project does not
configure `tessera-ui` is not version-checked; a project that configures the package, and declares
a `version`, gets the check against the manifest in its own root.

## Requirement

`packages[].version` in `tessera.json` is optional:

| Form | Satisfied when the manifest version is |
|---|---|
| `X.Y.Z` | exactly `X.Y.Z` |
| `^X.Y.Z` | the same major as `X.Y.Z` and not older than it |

Caret follows the major only, also below `1.0.0`. A library at `0.x` signals compatible changes
with the minor, so an application pins such a library exactly until it reaches `1.0.0`. A package
entry without `version` is never checked and needs no manifest, which keeps plain local packages
free of ceremony.

## Check and diagnostics

The driver runs the check once, after reading `tessera.json` and before parsing any source. The
first failure stops the run with exit code 1 and one message in the form
`tessera.json:1:1: CODE: text`:

| Code | When |
|---|---|
| `TESSERA_PACKAGE_VERSION` | The requirement is not `X.Y.Z` or `^X.Y.Z`, or the manifest version does not satisfy it |
| `TESSERA_PACKAGE_MANIFEST` | The manifest is missing or unreadable, not JSON, lacks a string `name` or `version`, names another package, or its `version` is not `X.Y.Z` |

A `packages[].version` that is not `X.Y.Z` or `^X.Y.Z` is also a `tessera.json` read error.

## Tests

`tests/unit/codegen_cli_tests.cpp` runs the compiler over a fixture package: no version and no
manifest, a newer minor under a caret range, an exact match, a different patch, a different major,
an older patch, a missing manifest, a manifest naming another package and a malformed requirement,
plus the `tessera-ui/primitives` subpath.

## Versioning

The manifest is schema-less beyond the table above. Adding an optional field is not a version
change; changing the meaning of `name`, `version` or the requirement forms is a new spec version.
