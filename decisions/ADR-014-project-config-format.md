# ADR-014: Project configuration is a versioned `tessera.json` plus a C++ API

- Status: Accepted
- Date: 2026-11-07
- Related: ADR-007, ADR-008, ADR-012, ADR-013

## Context

Developers need one stable, tool-friendly place to configure a Tessera project: app identity,
UI sources, hooks, generated-code options, assets, build settings, and the browser preview.
The options are a JSON/TOML/YAML file, CMake variables, or a C++-only API.

The generator must be deterministic and the configuration must be readable by both the CLI
and the running application.

## Decision

1. **`tessera.json` at the project root is the source of truth.**
2. **Schema version 1.** `schemaVersion` is required; unknown fields are errors (strict schema).
3. **C++ API mirrors the schema.** `tessera::AppConfig::from_file()` /
   `from_json()` produce the same structure; `tessera::App` consumes it.
4. **CLI flags override for one invocation** (`--preset`, `--renderer`, `--no-hot-reload`) and
   never rewrite the file.
5. **Paths are relative to the project root.** Absolute paths are rejected.
6. **Codegen reads the validated config.** UI semantics never come from ad-hoc CMake variables.
7. **Deterministic generation:** config hash + source hash decide whether a component is
   regenerated; no timestamps or absolute paths in generated code.
8. **Schema is published** at `https://tessera.dev/schema/project-v1.json` when the docs site
   exists. Until then, `docs/specs/project-config-v1.md` is canonical.
9. **Versioning:** adding optional fields keeps v1; changing defaults/meaning or removing a
   field bumps the version and ships `tessera migrate`.

## Consequences

### Positive

- One config file for CLI, codegen, build, and runtime.
- Editors can validate against a JSON schema.
- Strict errors catch typos early.
- The C++ API and the file cannot drift silently.

### Negative / risks

- JSON has no comments; documentation must carry the explanations.
- Strict schemas add friction on upgrades; migrations are required.
- A schema change is a compatibility event, not a casual edit.

## Alternatives considered

- **TOML.** Nicer for humans, but another parser dependency and weaker editor-schema support.
- **YAML.** Flexible but error-prone for configuration and heavy to parse.
- **CMake variables only.** Rejected: not a developer-facing API; poor tooling and discoverability.
- **C++-only configuration.** Rejected: no declarative UI/project scaffold; worse for the
  design-time viewer and codegen.

## Follow-up

- Implement the validator and `AppConfig` in Big Step C.
- Publish the JSON schema.
- Add `tessera doctor` checks for schema, toolchain, Vulkan SDK, fonts, and static linking.
