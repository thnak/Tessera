# Compiler: JSX/TSX -> IR -> C++

Status: design for Big Step C. `src/compiler/{analyzer,rules,ir,codegen}` are placeholders;
the protected-region merger exists in B1.

The compiler is build-time only. It never ships in a production app and never executes
arbitrary JavaScript from components.

## 1. Goals

- Accept standard React/JSX syntax for the supported subset.
- Reject unsupported constructs loudly with source locations.
- Produce a neutral, versioned IR (ADR-007).
- Generate readable, deterministic C++ (one pair per component).
- Preserve developer C++ inside protected regions (ADR-008).
- Be incremental and cache-friendly.

## 2. Pipeline

```text
tessera.json
  -> validate config
  -> load/validate ui/elements.json + shader manifest
  -> discover ui.sourceDirs/include/exclude
  -> own JSX/TSX front end (lexer/parser, ADR-036)
  -> Tier 1: React rules
  -> Tier 2: bindings + node lowering
  -> IR (docs/specs/ir-v1.md)
  -> IR validation
  -> codegen
  -> clang-format (pinned)
  -> protected-region merge
  -> bindings manifest + report
```

Each stage has a stable diagnostic code and source span. A component that fails validation is
not partially generated.

## 3. Front end (own parser)

Decision: ADR-036. Tessera has its own lexer/parser for a defined JSX/TSX subset; SWC, oxc,
tree-sitter, and Node.js are not used.

- input: `.tsx`/`.jsx`, restricted to the subset defined in
  [`jsx-subset-v1.md`](../specs/jsx-subset-v1.md)
- output: a normalized module AST (components, imports, JSX nodes, expressions, source spans)
- C++, static, deterministic; stable diagnostic codes, spans, and suggested fixes
- unsupported syntax is a hard error naming the construct (no best-effort parsing)
- no parser AST type may leak into the IR or codegen; the IR does not depend on the parser
- the same front end library is linked into the native CLI (C++ code generation) and into the
  browser designer's Wasm engine (live view, `decisions/ADR-037-self-contained-wasm-designer.md`),
  so both produce the same IR; it must be portable C++ (no OS/thread/file
  assumptions) and support error-tolerant, component-granular parsing for live typing
- tests: corpus, fuzzing, diagnostics goldens, byte-identical determinism

Fidelity: the model is React-like, not React-identical. The compiled UI aims to look like the
web version for the supported subset (see the stylesheet compatibility matrix), but parity with
React + browser is a goal, not a guarantee.

## 4. Tier 1: React rules

Validate before lowering:

- valid JSX/TSX and imports
- hooks called unconditionally at the top level, stable order
- allowed hooks: `useState`, `useEffect`, `useRef`, `useAnimatedValue` plus built-in library
  hooks declared in the built-in hook manifest (typed signatures, same ordering rules; ADR-036)
- no user-defined custom hooks, context, suspense, portals, or dynamic component types
- error boundaries are supported wrappers; codegen guards boundary entry points
- state setters are used in event handlers/effects, not during render
- effects have static dependency arrays; no missing/extra deps (V1: exact matching)
- conditionals/loops are statically analyzable (no arbitrary runtime expressions)
- no side effects in render
- binding annotations `// Note: bind::<id>` are unique and well-formed

Unsupported patterns are hard errors with `TESSERA_JSX_*`/`TESSERA_REACT_*` codes.

## 5. Tier 2: lowering

- Resolve imports and component references.
- Map JSX intrinsics to IR node kinds (`View`, `Text`, `Button`, `Image`, `Path`, `Scroll`,
  `CustomElement`).
- Map props/style/events to IR fields.
- Resolve `bind::<id>` annotations to binding records and protected-region ids.
- Lower static conditionals/loops to IR condition/loop nodes.
- Preserve source spans for diagnostics and source maps.

The IR is the only input to codegen. Codegen never reads the parser AST.

### 5.1 Custom element lowering

- `<CustomElement element="X" .../>` requires a literal `element`; resolve it against
  `ui/elements.json`.
- Validate props/events against the element schema; reject unknown/missing fields, children,
  and non-literal/dynamic element ids.
- Resolve `shaders`/`pipeline`/`render`/`input`/`test` capabilities from the element manifest;
  validate shader refs against the shader manifest.
- Emit an IR `CustomElement` leaf with `element`, typed props, event bindings, and `uiId`.
- Generate `generated/elements.g.cpp` (compile-time factory registry) and
  `generated/elements.d.ts` (typed JSX props/events).
- No runtime registration, dynamic plugin loading, or TSX parsing at runtime.
- See [`custom-elements-v1.md`](../specs/custom-elements-v1.md).

## 6. IR

See [`ir-v1.md`](../specs/ir-v1.md). Requirements:

- serializable to JSON for snapshots/debugging
- versioned (`irVersion: 1`)
- deterministic ordering (components by path, nodes by source order, bindings by id)
- explicit references and types; no raw JS expressions
- source spans on components, nodes, bindings, and diagnostics

## 7. Codegen

- One `<Component>.g.h` + `<Component>.g.cpp` per component.
- Header comment: `// Generated by Tessera. Do not edit outside protected regions.`
- Stable namespace (`<app.namespace>::gen` by default).
- Construct scene nodes, bind styles/props/events, emit state/effect/ref/animated-value
  members and lifecycle stubs.
- Developer logic lives only inside protected regions.
- `ErrorBoundary` emits a guard around child build/update/effect/event entry points; the
  fallback and `onError` are outside that guard and propagate to the parent/root on failure.
- Assign `ComponentId`/`UiId` deterministically; emit `generated/automation.manifest.json`.
  Node constructors carry `(NodeId, UiId, ComponentId)`; collisions are hard errors.
- Emit `generated/elements.g.cpp` and `generated/elements.d.ts` from `ui/elements.json`;
  `CustomElement` construction goes through the generated factory.
- No timestamps, absolute paths, or random ids.
- Compile with `/W4` (MSVC) and `-Wall -Wextra -Wpedantic` (GCC/Clang).
- Generated code includes only public Tessera headers and its own generated headers.

## 8. Formatting and merging

- `clang-format` is pinned and applied before merging (ADR-008/015).
- Marker placement/indentation must be stable.
- Merge algorithm per ADR-008:
  - both: keep developer body
  - generated only: keep generated stub
  - existing only: keep file, warn
  - duplicates/mismatch/missing end: hard error
- Idempotency test: `merge(merge(g,e),e) == merge(g,e)`.

## 9. Incremental generation

Hash:

- component source
- relevant `tessera.json` sections
- generator/compiler version
- formatter config
- IR schema version

Unchanged components are skipped. `--force` regenerates everything. `--check` fails if the
working tree differs from generated output. The report lists generated/skipped/merged/warnings.

## 10. Diagnostics

- Codes: `TESSERA_CFG_*`, `TESSERA_JSX_*`, `TESSERA_REACT_*`, `TESSERA_IR_*`,
  `TESSERA_CG_*`, `TESSERA_MERGE_*`.
- Every diagnostic has severity, code, message, file/line/column, and optional fix.
- Build-time errors fail the build; warnings are summarized and can be treated as errors in CI.
- Runtime never sees compiler diagnostics; they are build artifacts.

See [`error-model-v1.md`](../specs/error-model-v1.md).

## 11. Testing

- Parser fixtures: valid/invalid JSX/TSX, imports, escapes.
- Tier 1 fixtures: hook order, unsupported hooks/context/suspense, dynamic components.
- Custom elements: unknown/literal element, prop/event schema, leaf rule, shader refs,
  generated registry/typings.
- IR snapshots: stable JSON for representative components.
- Codegen goldens: generated C++ text after formatting.
- Compile tests: generated code builds with the supported compilers.
- Merger: duplicate/missing markers, CRLF, idempotency, rename warnings.
- Incremental: unchanged inputs skip; config/source change regenerates.
- Determinism: repeated runs produce byte-identical output.

## 12. Open questions

- Built-in hook manifest format and the open questions listed in
  [`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 18.
- Parser error recovery for the live view (editing partially valid files).
- Source maps from generated C++ back to TSX.
- Incremental codegen at the node level vs component level.
- How much static analysis for effect dependencies.
- Error recovery for partially valid files (V1: fail fast).
- Migration tooling for binding renames.
