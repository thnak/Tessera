# Security and privacy

Status: design. Applies to the compiler, runtime, designer (browser preview tool, [`designer.md`](designer.md)), services, and supply chain. Abbreviations: PII = personally identifiable information; TLS = transport layer security; CSP = Content Security Policy; Wasm = WebAssembly; FFI = foreign function interface; JNI = Java Native Interface; RHI = render hardware interface ([glossary](../reference/glossary.md)); SBOM = software bill of materials.

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
- **Components are not arbitrary JavaScript.** The compiler accepts a fixed JSX/hooks subset ([ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md), own JSX front end);
  production never executes source JS.
- **The Wasm designer is sandboxed.** No native filesystem/network access; the host bridge is
  local-only and validates messages.
- **OS/GPU are trusted system components** but may return malformed data; validate sizes,
  formats, and handles.
- **Network data is untrusted.** TLS verification, size limits, timeouts, and schema validation.

## 3. Compiler and codegen

- Parser runs build-time, but must be robust to malformed/hostile input (fuzz it).
- Generated C++ identifiers/strings are escaped; user text cannot inject code.
- Binding ids (stable `bind::<id>` names linking JSX to protected C++ regions, see [glossary](../reference/glossary.md)) are validated (`[A-Za-z0-9_.:-]+`); duplicates/mismatches are errors.
- Generated paths stay under `codegen.outDir` ([`project-config-v1.md`](../specs/project-config-v1.md)); symlinks are rejected/resolved safely.
- Resource limits: enforced by the front end per section 3.1; loop depth is validated in the IR
  ([`ir-v1.md`](../specs/ir-v1.md) section 5).
- No network access during generation unless explicitly configured.
- Deterministic output prevents hidden code changes.

### 3.1 Enforced front-end limits

The JSX/TSX front end is the first parser that sees untrusted project files. It enforces the
limits of [`jsx-subset-v1-jsx.md`](../specs/jsx-subset-v1-jsx.md) section 15 with stable
`TESSERA_JSX_LIMIT_*` codes; exceeding one is a diagnostic, never a crash, a hang or a stack
overflow. The defaults live in
`src/compiler/analyzer/include/tessera/compiler/jsx/limits.hpp` and are configurable
([`project-config-v1.md`](../specs/project-config-v1.md) `ui.limits.*`).

| Limit | Default | Code | Enforced in |
|---|---|---|---|
| File size | 1 MiB | `TESSERA_JSX_LIMIT_FILE_SIZE` | `Lexer` constructor, before any scan |
| Tokens per file | 200000 | `TESSERA_JSX_LIMIT_TOKENS` | `Lexer::next`/`continue_template`; the lexer returns `Eof` forever after |
| Nesting depth | 128 | `TESSERA_JSX_LIMIT_DEPTH` | `Parser::enter()` (recursion) and `finish_node()` (AST height of expressions, elements and types) |
| Diagnostics per file | 100 | `TESSERA_JSX_LIMIT_DIAGNOSTICS` | `DiagnosticSink::report`; later errors are counted, not stored |
| Components per file | 64 | `TESSERA_JSX_LIMIT_COMPONENTS` | `Parser::parse_module_root` |

Two properties make those limits hold under hostile input:

- **Recursion and AST height are both capped.** Depth is counted at every recursive production
  (parentheses, calls, arrows, conditionals, templates, arrays/objects, JSX elements and
  attributes, type generics/objects/functions, and the expressions and types of module items)
  *and* stored as node height, so a long left-associative operator or member chain counts like
  nesting. Deleting a tree therefore cannot overflow the stack, and the stored height is the
  precondition the downstream stages rely on.
- **Diagnostic text is bounded.** Messages quote input through `excerpt()` (at most 32 bytes, cut
  at a code point boundary) and `DiagnosticSink` caps message/suggestion/related text at
  256 bytes, so a 1 MiB line cannot produce a diagnostic that grows with the file. The report is
  at most `max_diagnostics + 1` entries; after the cap, the file-wide UTF-8/CR pre-scan stops
  early because further reports would be dropped.

The regression net is the fixed corpus in `tests/unit/compiler_limits_tests.cpp` (deep nesting,
10k siblings, a ~1 MiB file, pathological expression/type/module chains, invalid UTF-8, and
every limit boundary) plus `tests/unit/jsx_adversarial_tests.cpp` (prefixes, mutations, random
bytes). A new limit needs a case in that corpus.

### 3.2 Checks the downstream stages must add

The front end guarantees a bounded AST height and token count. The IR, lowering and codegen
stages (`src/compiler/{ir,lowering,codegen}/`) must not rely on that alone: IR also arrives from
`ir_json`, tests and future front ends. Add these checks before recursing or emitting:

| Stage | Check to add |
|---|---|
| `ir::validate` | One iterative pre-pass with an explicit stack that computes the maximum node depth and the node count, and rejects depth > 128 (`TESSERA_IR_LIMIT_DEPTH`) or nodes > 200000 (`TESSERA_IR_LIMIT_NODES`) **before** the existing recursive walk. |
| `ir_json` decode | Reject nesting deeper than the same depth budget while parsing, so a hostile module never materializes an over-deep tree. |
| lowering | Recompute the height of the lowered node tree iteratively instead of trusting `Expr::height`/`JsxNode::height`: `.map` loops, `Children` slots and component composition can deepen the tree. |
| codegen | Cap one `GeneratedFile::text` at 8 MiB and the whole file set at 64 MiB while emitting (`TESSERA_CODEGEN_LIMIT_BYTES`); reject identifiers and `bind::` ids longer than 256 bytes before they become C++ identifiers (`TESSERA_CODEGEN_LIMIT_IDENTIFIER`); keep every user string on the existing escaper path. |

A limit that can only be enforced downstream must be added there with its own stable code and a
case in that stage's tests; it must not be silently dropped.

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

## 4.1 Custom native elements

- Custom elements ([ADR-028](../../decisions/ADR-028-custom-native-elements.md), custom native elements) are trusted native app code compiled into the binary; no untrusted native
  loading, no `dlopen`, no JS/eval.
- `ui/elements.json` is untrusted project input: validate ids, class/source paths, prop/event
  schemas, shader refs, formats, and sizes; reject traversal/unknown values.
- Shaders/pipelines are build-time compiled/reflected; no runtime shader source from project
  data. Pipeline state must match the shader manifest.
- Elements use the RHI, not raw Vulkan/WebGPU/OS handles; validate resource sizes/formats and
  budgets.
- `on_render` returns `Error` (no throw across threads/ABI); update/input exceptions go to the
  nearest error boundary; device loss disables/recreates safely.
- No file/network/OS calls in render; test hooks are debug/headless-only. No secrets/PII in
  snapshots/traces/goldens. See [`custom-elements-v1.md`](../specs/custom-elements-v1.md).

## 5. Designer/Wasm

- Browser sandbox + CSP; no `eval` of project source.
- Host bridge binds localhost only; authenticate/validate the session.
- IPC messages are versioned and schema-validated; reject unknown/oversized payloads.
- No arbitrary filesystem paths; assets are served from the project root allowlist.
- WebGPU validation errors are diagnostics; do not expose raw host capabilities.

## 6. Services and data

- TLS verification on by default; no insecure fallback.
- Secrets/tokens in the platform keystore; never in config/logs. Keystore availability per platform and the abstraction over it are not specified (open question, section 11). Auth/session
  startup behavior is in [`startup-and-loading.md`](startup-and-loading.md).
- Redact headers, query params, file paths, and user data in logs/crash dumps.
- Validate deep links/notification payloads; allowlist schemes/hosts/routes. See
  [`deep-linking-v1.md`](../specs/deep-linking-v1.md).
- Clipboard/drop/file-dialog payloads are untrusted; validate paths/types/sizes. See
  [`platform-shell-v1.md`](../specs/platform-shell-v1.md).
- Secure text fields never log/clipboard/persist plaintext. See
  [`text-editing-v1.md`](../specs/text-editing-v1.md).
- Storage paths are sandboxed; atomic writes prevent corruption.
- Telemetry opt-in, aggregated, and documented ([ADR-022](../../decisions/ADR-022-diagnostics-and-profiling.md), diagnostics and profiling); no PII by default.
- Updates verify TLS/signature/hash, stage atomically, and roll back safely; see
  [`updates-and-distribution-v1.md`](../specs/updates-and-distribution-v1.md).
- Crash reports are opt-in, redacted, and symbol/version matched; see
  [`crash-reporting-v1.md`](../specs/crash-reporting-v1.md).

## 7. Supply chain and licensing

- Pin dependency versions/hashes; generate an SBOM.
- Static-link allowlist: permissive licenses only ([ADR-012](../../decisions/ADR-012-static-linking.md), static linking; [ADR-013](../../decisions/ADR-013-project-license-mit.md), MIT project license); no GPL/LGPL in the core.
- Preserve notices (`THIRD_PARTY_NOTICES.md`, Apache NOTICE file, SIL Open Font License, FreeType License, ICU/Unicode license).
- CI checks the link graph for unexpected dynamic libraries.
- Review new dependencies for maintenance, CVEs, and build reproducibility.
- Do not fetch arbitrary code at build time without pinning.

## 8. Diagnostics privacy

- Default logs exclude secrets/PII; redaction is tested.
- Crash dumps are opt-in/consent-based where required; bounded and redacted.
- Frame captures/screenshots may contain user data; require explicit action.
- Telemetry is opt-in and never includes raw UI content.
- Log levels/sinks are configurable ([`diagnostics-config-v1.md`](../specs/diagnostics-config-v1.md)); the release defaults are listed there (this file does not restate them).

## 9. Vulnerability response

- Private disclosure channel; acknowledge and triage.
- Reproduce with a minimal project + deterministic mode.
- Patch, add a regression test, and publish an advisory (publication criteria are not specified).
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

- Sandboxing the compiler process (Linux seccomp / Windows Job Objects, OS process-restriction features) vs in-process.
- Signature/verification for project assets and updates.
- Wasm bridge authentication model for remote preview.
- Secret storage abstraction across desktop/mobile.
- SBOM format and distribution.
