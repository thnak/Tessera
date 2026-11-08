# ADR-037: Self-contained Wasm designer engine with picker and style editing

- Status: Accepted
- Date: 2026-11-08
- Related (number: subject): ADR-002: WebGPU (Dawn) for the Wasm designer, separate from Vulkan;
  ADR-007: neutral IR; ADR-008: protected regions; ADR-012: static linking; ADR-022: structured
  diagnostics; ADR-024: no JS/React runtime in production; ADR-036: own JSX/TSX front end.
- Terms: **Wasm** = WebAssembly. **Front end**, **IR**, **live view**: see ADR-036. **Evaluator**
  = the component of the Wasm engine that executes IR directly (no C++ generation). **Picker** =
  selecting a component or node by clicking it in the preview. **Override layer** = a set of
  style values that the designer adds on top of the normal style cascade for preview only.
  **Span** = a start and end position in a source file. **Designer** = the browser tool described
  in `docs/design/designer.md`.

## Context

ADR-036 gave Tessera its own JSX/TSX front end and a live view. The first wording ("update a
running engine") left open how the live view executes user code and who runs the parser. The
intended experience is: open the designer, type or load UI code, and see it rendered
immediately, then click a component in the preview and style it. That must not require a
native build, a JS/React runtime, or a round trip to a native compiler for every keystroke.

## Decision

1. **The Wasm module is a statically compiled, self-contained engine.** "Statically compiled"
   means a fixed binary: it is built once per Tessera release, not once per user edit, and contains: the Tessera JSX/TSX front end (lexer, parser, analyzer), IR lowering,
   an IR evaluator, the UI/style/layout/text engine, the batched 2D renderer, and the WebGPU
   RHI backend. User UI code is never executed by JavaScript and the Wasm module is never
   recompiled per edit. JavaScript is only the shell (canvas, DOM panels, editor, file I/O).
2. **Parse in the engine, render immediately.** Source text -> front end -> IR -> evaluator ->
   scene -> frame. The same front end library is linked into the native CLI (codegen) and the
   Wasm module (live view), so both consume one IR. The front end therefore must be portable
   C++ (no OS, thread, or filesystem assumptions).
3. **Incremental and non-destructive.** Re-parse only changed components; diff IR; patch the
   scene in place; preserve state by component identity + key. A failed parse/analysis keeps
   the last good IR per component and shows diagnostics; no blank preview.
4. **Evaluator parity.** The IR evaluator implements the same semantics as generated C++
   (hooks ordering, expression subset, event handlers). A differential conformance suite runs
   the same IR through the evaluator and through generated native code and compares scene
   snapshots and golden frames at fixed steps.
5. **Picker.** The engine supports hit-test picking, hover highlight, and tree-based selection
   of component instances and primitive nodes. Every scene node carries `(componentId,
   instanceKey, nodeId, sourceSpan)` from the IR, so a pick resolves to a source location.
   Highlights are drawn in a designer overlay layer that is excluded from semantics,
   automation, and goldens.
6. **Style editing.** The engine shows computed style, matched rules, and cascade origin for
   the selection. Edits apply first as a designer-only override layer in the cascade for instant
      validated by the stylesheet engine against the compatibility matrix in
   `docs/specs/stylesheet-v1.md` section 11.
   A commit turns the edit into a minimal source patch (a text edit that changes only the edited
   value and the syntax needed around it, leaving other text, formatting and comments unchanged) for one of three
   destinations: the element's inline `style`, a class rule in a CSS file, or a theme token. The
   patch is sent to the host/editor to apply; the file change then round-trips
   through the front end to the same IR and clears the override. Undo/redo live in the designer.
7. **The engine never writes files.** Patches go to the host or editor via the local IPC
   bridge. Patches touch only UI sources (TSX/CSS/tokens), never developer C++ in protected
   regions (ADR-008).
8. All designer/inspector/mutation code is dev-only and compiled out of production builds
   (ADR-024); mutation commands are explicit and local-only (consistent with
   `devtools-protocol-v1.md`).

## Consequences

- Instant feedback with no JS runtime and a single semantic implementation of the front end.
- The Wasm module becomes larger because it includes the front end and the evaluator. A size and
  startup-time budget has not been set; it must be set and measured (see `designer.md` section 10).
- Two executors of IR semantics (evaluator and generated C++) require differential tests to
  prevent preview/native drift; unsupported-in-evaluator constructs must be rejected at the
  front end, not at runtime.
- Style editing needs stable source spans and a span-preserving patch writer; formatting and
  comments in user files must survive edits.
- The front end needs error-tolerant behavior for live typing (component-granular last-good
  fallback) beyond the fail-fast codegen mode.
- Native host remains for the filesystem, CLI codegen, and assets; IPC shrinks to files,
  diagnostics, and patches.

## Alternatives

- Run user code in browser JS with a React reconciler: rejected (ADR-024/036, preview drift).
- Native host compiles on every change and streams IR/C++ to a thin viewer: rejected as the
  primary path (latency, requires a native toolchain); kept as the codegen path.
- Recompile C++ to Wasm per edit (Emscripten in the loop): rejected (seconds of latency).
- Read-only inspector with source-only styling: rejected (no designer workflow).

## Follow-up

- Specify `docs/specs/designer-protocol-v1.md` (pick/selection/style/patch messages, overlay).
- Add source-span and component identity requirements to `ir-v1.md`.
- Set Wasm size/startup budgets; decide parse placement (main thread tick vs Web Worker).
- Differential evaluator-vs-native suite; span-preserving patch writer tests.
