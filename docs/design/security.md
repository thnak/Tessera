# Security and privacy

Status: design. Applies to the compiler, runtime, designer, services, and supply chain.

Tessera processes untrusted project files, assets, and network data, and ships as a statically
linked native executable. The security model is least privilege, strict validation at
boundaries, and no arbitrary code execution from UI sources.

## 1. Threat model

| Asset | Threat |
|---|---|
| Project files (`tessera.json`, TSX/CSS/assets) | path traversal, malicious config, decompression bombs |
| Compiler | parser crashes, code injection into generated C++, resource exhaustion |
| Runtime | malformed input/OS events, GPU driver bugs, unsafe FFI |
| Designer/Wasm | browser bridge abuse, untrusted project code, CSP bypass |
| Services | TLS downgrade, token leakage, malicious deep links/notifications |
| Supply chain | compromised dependency, license violation, unpinned tool |
| Diagnostics | secrets/PII in logs/crash dumps |

## 2. Trust boundaries

- **Project files are untrusted input.** Validate schema, paths, globs, asset sizes, and
  generated identifiers. Reject absolute paths and `..` traversal.
- **Components are not arbitrary JavaScript.** The compiler accepts a fixed JSX/hooks subset;
  production never executes source JS.
- **The Wasm designer is sandboxed.** No native filesystem/network access; the host bridge is
  local-only and validates messages.
- **OS/GPU are trusted system components** but may return malformed data; validate sizes,
  formats, and handles.
- **Network data is untrusted.** TLS verification, size limits, timeouts, and schema validation.

## 3. Compiler and codegen

- Parser runs build-time, but must be robust to malformed/hostile input (fuzz it).
- Generated C++ identifiers/strings are escaped; user text cannot inject code.
- Binding ids are validated (`[A-Za-z0-9_.:-]+`); duplicates/mismatches are errors.
- Generated paths stay under `codegen.outDir`; symlinks are rejected/resolved safely.
- Resource limits: file size, component count, nesting depth, loop depth.
- No network access during generation unless explicitly configured.
- Deterministic output prevents hidden code changes.

## 4. Runtime

- Validate pointer/keyboard/touch/IME payloads and OS event sizes.
- No `reinterpret_cast` from untrusted byte buffers without bounds/layout checks.
- GPU resource creation validates dimensions/formats; avoid integer overflow.
- Shader/pipeline creation is build-controlled; no runtime shader compilation from user data.
- FFI/JNI boundaries validate object lifetimes and exceptions.
- Crash handlers do not execute attacker-controlled code; dumps are bounded/redacted.
- Error-boundary fallbacks must not display raw stack traces, secrets, or user data; see
  [`error-boundaries-v1.md`](../specs/error-boundaries-v1.md) and
  [`error-model-v1.md`](../specs/error-model-v1.md).

## 5. Designer/Wasm

- Browser sandbox + CSP; no `eval` of project source.
- Host bridge binds localhost only; authenticate/validate the session.
- IPC messages are versioned and schema-validated; reject unknown/oversized payloads.
- No arbitrary filesystem paths; assets are served from the project root allowlist.
- WebGPU validation errors are diagnostics; do not expose raw host capabilities.

## 6. Services and data

- TLS verification on by default; no insecure fallback.
- Secrets/tokens in platform keystore where available; never in config/logs. Auth/session
  startup behavior is in [`startup-and-loading.md`](startup-and-loading.md).
- Redact headers, query params, file paths, and user data in logs/crash dumps.
- Validate deep links/notification payloads; allowlist schemes/hosts.
- Storage paths are sandboxed; atomic writes prevent corruption.
- Telemetry opt-in, aggregated, and documented; no PII by default.

## 7. Supply chain and licensing

- Pin dependency versions/hashes; generate an SBOM.
- Static-link allowlist: permissive licenses only (ADR-012/013); no GPL/LGPL in the core.
- Preserve notices (`THIRD_PARTY_NOTICES.md`, Apache NOTICE, OFL/FTL/ICU).
- CI checks the link graph for unexpected dynamic libraries.
- Review new dependencies for maintenance, CVEs, and build reproducibility.
- Do not fetch arbitrary code at build time without pinning.

## 8. Diagnostics privacy

- Default logs exclude secrets/PII; redaction is tested.
- Crash dumps are opt-in/consent-based where required; bounded and redacted.
- Frame captures/screenshots may contain user data; require explicit action.
- Telemetry is opt-in and never includes raw UI content.
- Log levels/sinks are configurable; release defaults are conservative.

## 9. Vulnerability response

- Private disclosure channel; acknowledge and triage.
- Reproduce with a minimal project + deterministic mode.
- Patch, add a regression test, and publish an advisory when appropriate.
- Rotate any exposed secrets; review logs/artifacts.
- Update dependencies and regenerate notices/SBOM.

## 10. Tests

- Path traversal/symlink/oversized asset tests.
- Compiler fuzzing (parser/IR/codegen) with bounded inputs.
- Codegen injection tests (identifiers/strings).
- IPC malformed/oversized message tests.
- TLS failure/redirect/secret-redaction tests.
- Link-graph/license checks in CI.
- Crash-dump redaction tests.

## 11. Open questions

- Sandboxing the compiler process (seccomp/Job Objects) vs in-process.
- Signature/verification for project assets and updates.
- Wasm bridge authentication model for remote preview.
- Secret storage abstraction across desktop/mobile.
- SBOM format and distribution.
