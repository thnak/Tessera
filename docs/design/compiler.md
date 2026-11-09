# Compiler: JSX/TSX -> IR -> C++

Status: design for Big Step C. `src/compiler/{rules,ir,codegen}` are placeholders; the
protected-region merger exists in B1, and the JSX/TSX lexer and module/element/expression parser are in
`src/compiler/analyzer` ([`jsx-frontend.md`](jsx-frontend.md)).

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
  hooks declared in the built-in hook manifest (typed signatures, same ordering rules; ADR-036;
  format in [`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 7.1)
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

## 11. C++ codegen (first slice, implemented)

`tessera_codegen` (`src/compiler/codegen/`, links `tessera_ir` only) emits one `.hpp` and one `.cpp`
per IR component into the module namespace (`<module.name>::gen`), sorted by component name and
byte-identical for the same IR (two runs and eight threads; golden-tested).

Per component: a struct with one field per `useState`, `useRef`, `useAnimatedValue` and prop (IR
types mapped to `bool`/`int`/`double`/`std::string`/`tessera::ui::Node*`/`std::vector`/
`std::optional`); `build(tessera::ui::Node& parent)` constructing the static intrinsic tree with
`node::N` order (the IR `uiId` when it is numeric, otherwise a pre-order placeholder until slice
10) and attaching it with `add_child`; a method per inline handler record (setter actions assign the
state cells); and one merger-protected `[DEVELOPER_LOGIC_START: bind::<id>]` region per binding
(event handlers take `const tessera::ui::Event&`, effect/cleanup bindings are `void` stubs). Merger
idempotency is re-verified over generated output: `merge(merge(g,e),e) == merge(g,e)`.

Expressions: literals (string escaping, float shortest round-trip), `state::`/`prop::`/`ref::`/
`anim::` references as generated fields, `! - +` unary, `== != < <= > >= + - * / && ||` binary (`/`
emits an explicit double cast because IR division is float), `?:`, text templates whose parts are
all text (folded to one string literal), and calls to `min max clamp abs floor ceil round toInt
toFloat`. An IR `array` expression is initializer-only: a state/prop initial with literal elements
is emitted as a braced C++ list (`std::vector<std::string> items_ = {"a", "b"};`), and in any other
expression position it is rejected loudly. Rejected loudly with `TESSERA_CODEGEN_UNSUPPORTED`:
record-field references (`prop::row.x`,
`event.value`), `%` (int vs float remainder needs slice 4), text templates with substitutions,
`length`/`formatNumber`, `env::` references, record-typed fields,
`Component`/`CustomElement`/`VirtualList`/`VirtualGrid` nodes, and `environment` hooks.

Scene APIs in use, in the documented per-node emission order: `set_element_kind`,
`set_element_id`, `set_class_name`, `set_automation_id`, `set_role`, `set_aria` (`aria-*` in name
order), event bindings, focusability (`Button`/`TextInput`/`TextArea`), scroll flags from
`Scroll.axis`, D9 text via `set_paint_data(PaintData{...})`, then the child subtrees and `add_child`.
Element metadata is borrowed by the node ([`scene-graph.md`](scene-graph.md) section 5), so codegen
passes string literals only and rejects dynamic `id`/`className`/`role`/`aria-*`/text with
`TESSERA_CODEGEN_UNSUPPORTED`. Inline style values have no runtime API yet and remain deterministic
`// node::N style.x=...` comments.

Events: for every `node.events` entry codegen emits an anonymous-namespace adapter
`void event_adapter_<N>_<onName>(void* context, const tessera::ui::Event& event)` that casts the
component context, converts the scene event with the payload-record converter, and calls the
generated handler/binding method; the node then receives
`bind_event(tessera::ui::EventType::<Type>, &adapter, this)`. Payload record → C++ type:
`PointerEvent`→`PointerEventInfo`, `ClickEvent`→`ClickEvent`, `KeyEvent`→`KeyEventInfo`,
`FocusEvent`→`FocusEventInfo`, `TextChangeEvent`→`TextChangeEvent` (`TextSubmitEvent` is an alias),
`TextSelection`→`TextSelection`, `CompositionEvent`→`CompositionEvent`,
`ScrollEvent`→`ScrollEventInfo`, `ErrorInfo`→`ErrorInfo`. JSX event → `EventType`: `onClick` →
`PointerUp` (click synthesis is later); `onPointerDown/Up/Move/Over/Out/Enter/Leave` → the same
scene type; `onKeyDown/onKeyUp` → `KeyDown/KeyUp`; `onFocus/onBlur` → `Focus/Blur`;
`onChange/onSubmit` → `EditText`; any other event name is rejected with
`TESSERA_CODEGEN_UNSUPPORTED`. Bindings are read-only and run at the Target phase after the node's
own listener and before bubbling ([`scene-graph.md`](scene-graph.md) section 6).

Refs and lifecycle: after the target node is created (before it is attached), `build()` assigns
`this->ref_<name>_ = node_N.get()`, so every `useRef` points at its IR target. Each `useEffect`
gets `tessera::ui::EffectState` (`component_lifecycle.hpp`) plus a generated `dep_hash_<i>()` FNV-1a
over its dependency members (state/prop/animated values; no allocation). The generated
`mount()` calls `update_effects()`, which invokes `effect_needs_run()` (false while active with the
same deps hash) and runs the cleanup region before rerunning when the hash changed;
`unmount()` runs cleanups in reverse declaration order and clears `active`. The effect bodies and
cleanups are the normal `[DEVELOPER_LOGIC]` binding regions, so protected-region idempotency covers
them. `useAnimatedValue` remains a plain field: `ui::AnimationRuntime` is target/name keyed
(`start`/`pause_target`/`tick`) and has no per-value controller API to hand a generated field to —
the exact gap for a later animation slice.

Interpolation and update path: text expressions (templates with substitutions, plain references,
conditionals and numeric/binary/call expressions with a printable C++ type) are emitted as a
generated `apply_state()`: it hashes each interpolated value with the shared FNV-1a helpers and only
when a hash differs rebuilds the string, stores it in `text_node_N_value_`, and calls
`set_paint_data(PaintData{...})` — so the node marks `Paint|Render` only, never `Layout`. `build()`
stores each interpolated node's pointer and ends with `apply_state()`. Generated setters and event
binding developer regions raise a component-level `state_dirty_` flag instead of touching the node
directly; `update()` = `update_effects(); apply_state(); state_dirty_ = false;` is the host entry
after event dispatch. Tradeoff (documented): setters stay cheap and synchronous-total (no effects
inside a setter), the host must call `update()` after dispatch (React-like scheduling), and the
current headless demo keeps its existing CPU goldens because a click alone marks no node dirty until
`update()`. Unchanged values allocate nothing (hash path only); a changed value builds one bounded
string and one `PaintData` copy. Templates with substitutions can appear directly or inside a text
conditional (`{count === 0 ? "Counter" : `Count: ${count}`}`), which emits branching hash/append
statements rather than a temporary string. Formatting: `true`/`false`, decimal integers, shortest
round-trip doubles via `std::to_chars`, strings verbatim; text is a paint payload, never a layout
input. Controlled `TextInput`/`TextArea` fields are generated too: the component owns an
`EditController`, a `TextInputRuntime` and a `TextChangeBinding` per field; `build()` attaches the
runtime, binds the six source input events plus the typed `TextChange` binding, and the seeded
`value`/`defaultValue` is painted (literal inline, interpolation through `apply_state`). An inline
`onChange={setName}` setter becomes `state_name_ = std::string(e.value); apply_state();`; a
controlled `onChange` that does not bind a `TextChangeEvent` payload is rejected loudly. Whenever
the controlled value changes, `apply_state()` updates `PaintData` and syncs the editor buffer only
when its text differs (external state edits; a typing commit already matches the buffer). Dynamic
element metadata (`id`, `className`, `role`, `aria-*`) is interpolated the same way:
the generated component owns the current string (node metadata stays a borrowed view), hash-compares
it, and only on change moves the old buffer out, assigns the new value, calls the node setter
(`set_element_id`/`set_class_name`/`set_role`/`set_aria`) and, for `id`/`className`, explicitly marks
`Dirty::Style` (SSO in-place updates can otherwise make the node's no-op compare skip the mark).
`automationId` stays literal-only because lowering requires a string literal today; other dynamic
intrinsic props and inline style remain gaps.

**Remaining missing runtime API / gaps:** a per-value animation controller for `useAnimatedValue`
hand-off; dynamic `automationId` (lowering requires a string literal today) and dynamic
non-metadata intrinsic props; sample `automationId` emission (numeric `automationId` is emitted
when the IR has it).

**Dynamic child sets: conditional branches and keyed lists (implemented).** Lowering emits a
conditional child as a *sibling* node carrying `condition`, so codegen builds every branch subtree
once in `build()` into an ownership slot (`std::unique_ptr<tessera::ui::Node> branch_N_owned_`) and
attaches it on demand. `apply_state()` first calls one generated `apply_children_<p>()` per parent
with dynamic children. That walk keeps a running `position` in IR child order: a static child
advances it, a present conditional branch advances it after `insert_child(position, ...)`, and a
list advances it by its item count. A branch therefore lands at its source position without
rebuilding the trailing siblings. Each branch keeps an FNV-1a hash of its condition plus the last
presence flag, so an unchanged condition and presence touch no node API and mark nothing dirty; a
change attaches/detaches through `insert_child`/`remove_child`, which mark the parent
`Layout|Paint|Render` (the child set changed) and never `Style`. A detached branch stays alive in
its slot (no re-allocation when it reappears) and keeps its stable `node::N` id. Nested conditionals
are supported. Damage contract: a detached branch's own bounds are not individually damaged because
`DamageTracker` never sees a detached node, but the shared parent is marked `Layout|Paint|Render`, so
the tracker damages its previous *and* current bounds, which contain the vacated in-flow branch. The
residual un-damaged case is a branch that overflows its parent's box (deferred; a full-target redraw
covers it). A conditional inside a list template is deferred (it needs per-item presence state).

A list template (a node carrying `loop` + `key`) generates a bounded item table
(`kMaxGeneratedListItems = 32`): `Item_<n>` per slot (item subtree root plus its interpolated text
pointers), the key strings, per-item content hashes, `count` and `order_hash`. `apply_list_<n>(parent,
at)` hashes the key list first (no allocation, no formatting, no node access when unchanged) and only
on a changed hash detaches the current items, matches new keys against the previous key strings,
reuses a matched node (with its text pointers and content hash), builds one node for a new key via
`build_item_<n>(item, index, out)`, re-inserts everything in order with `insert_child`, and clears the
stale slots. The per-item content pass `apply_item_<n>(slot, item, index)` hash-compares the item's
interpolated values and repaints only changed items through `PaintData` (`Paint|Render` only). Item
node ids are the template's pre-order id plus `index * kItemIdStride`, where `kItemIdStride` is one
past the component's last pre-order id, so every generated instance has a unique `NodeId` for damage
tracking and automation. The loop source must be a `state::`/`prop::` reference with a declared
`list<T>`; supported element types are the printable primitives (`bool`, `int`, `float`, `length`,
`string`, `color`); the key must read the loop item or index. Rejected loudly with
`TESSERA_CODEGEN_UNSUPPORTED`: record element types, nested list templates, conditionals /
`TextInput` / `TextArea` / dynamic metadata / dynamic inline style / `useRef` targets inside a
template, and list sources without a `list<T>` type. A source longer than the cap materializes the
deterministic prefix (documented truncation, no hidden growth).

Regeneration CLI (`tools/codegen/`, executable `tessera_codegen`): `.tsx` in, `--out DIR`
plus `--check`/`--write` (default), `--namespace`, `--verbose`; runs parser → analysis → lowering →
IR validation → codegen, first-error-only with `file:line:column`, exit 0/1/2. Every generated file
is merged with the existing file (`merge(generated, existing)`) and rewritten only when the merged
bytes differ, so a no-op run leaves bytes and mtime untouched; `--check` reports drift (including
outside-region edits) and writes nothing. Slice 10 identity diagnostics (`componentId`/`uiId`/
`automationId` missing) are the only IR diagnostics the CLI filters, because lowering does not
produce them yet. `samples/compiler-demo/` proves the flow end to end: `Demo.tsx` +
committed developer seed `tests/golden/codegen/demo/Demo.cpp` with hand-written region bodies, regenerated into the build
dir and compiled as `tessera_compiler_demo` under warnings-as-errors; `codegen_cli_tests` asserts
idempotent runs, mtime no-op, region preservation and outside-region repair. `Demo.tsx` also carries
a conditional child (`{count > 0 && <Text>Positive</Text>}`), so the CLI sample exercises branch
emission end to end; a keyed list is authorable from `.tsx` with literal elements (see Deferred for what stays out).

**Generated-UI integration demo (headless, implemented):**
`tests/integration/generated_ui_tests.cpp` instantiates the generated `demo::gen::Demo`, builds it
into a scene, lays it out with Yoga, paints with `paint_scene` through the deterministic CPU canvas
(`CpuPaintSink`), dispatches synthetic pointer/text input, and compares committed CPU goldens
(`tests/golden/baseline/generated_ui/{before,after,editing}.png`, tolerance 0). It asserts the
generated `Button` `PointerUp` binding increments `state_count_`, the W3C capture/target/bubble
route, the `Paint|Render` dirty domains (never `Layout`) for the repaint, and byte-identical repeat
renders. It also asserts the generated `{count > 0 && <Text>Positive</Text>}` branch is built once
and starts detached (so the before frame and its layout are unchanged), attaches at its source
position after the click, marks the parent `Layout`, and is laid out by a second reflow before the
after frame. Run it with `ctest -R generated_ui`; regenerate the goldens with
`generated_ui_tests --write <font.ttf> tests/golden/baseline/generated_ui`. The demo is registered
from `tests/unit/CMakeLists.txt` because the root CMakeLists (Lead-owned) does not add
`tests/integration` yet. The demo drives the repaint through the generated `apply_state()`/`update()`
path (no manual interpolation): the click sets `Count: 1`, the committed EditText sets
`Name: Ada`, and `update()` runs the generated effect.
The editable-field frame is the text-editing integration: the demo focuses the generated
`TextInput`, runs a scripted edit sequence through `tessera_text_edit` (typing, Home/End/arrows,
Shift-selection, select-all, delete-selection), and publishes the controller's caret and half-open
selection byte range into `PaintData::decoration` (the text value itself is also published by the
demo - see the gap below). Paint consumes only byte offsets and resolves them into line-box
rectangles; the demo asserts the caret/selection rects are inside the input's content box, that the
caret sits at the right edge of the selection, that a later caret is to the right of an earlier one,
and that deleting the selection removes the highlight and collapses the caret. All frames are
painted with `FontPaintTextMetrics` (real face metrics + HarfBuzz advances) so decoration geometry
matches the CPU glyph renderer; the `editing.png` golden covers text + selection highlight + caret.

**Controlled text input contract (next compiler slice):** the generated `TextInput` still has no
controlled `value`/`onChange` wiring. Current behaviour, asserted by the demo: the generated
binding fires on `EventType::EditText` with the generic `tessera::ui::Event`; the `on_name`
developer region copies `event.text` into `state_name_`; `apply_state()` then refreshes the separate
`Name: {name}` Text node. The TextInput node's own painted text does not change unless the demo
publishes the edit buffer. To close it, codegen must emit:

- **Event record** `TextChangeEvent` (already defined in
  `include/tessera/ui/scene/event_payloads.hpp`; IR/schema record of the same name, primary member
  `value`): `value`/`target_value` are the new full UTF-8 text (borrowed for the dispatch),
  `selection_start`/`selection_end` are the selection range. Units must be fixed to **half-open
  UTF-8 byte offsets** (the units `PaintData::TextDecoration` consumes); the schema
  `TextSelection` row currently says grapheme offsets and must be aligned in the same slice.
- **Generated member** `void on_change(const tessera::ui::TextChangeEvent& event)` with a payload
  adapter bound to `EventType::EditText` (and to the runtime edit-controller commit for key edits),
  replacing the current generic-`Event` binding for `onChange`. Inline `onChange={setName}` lowers to
  a setter handler that assigns `state_name_` from `event.value` and calls `apply_state()`.
- **Timing**: fire once per committed edit - an EditText commit, a key-driven edit
  (Backspace/Delete/paste) or a composition commit - and never for preedit
  `CompositionUpdate`s or per-repeat keystrokes (the schema's `onChange` row already says "not while
  composing; the commit fires once").
- **Value feed**: lowering seeds `PaintData::text` from the `value`/`defaultValue` prop;
  `apply_state()` writes `state_<name>_` back into the TextInput's `PaintData::text` (controlled
  value), marking `Paint|Render` and never `Layout` for same-box text. The caret/selection stay
  runtime edit-controller state and are published as `PaintData::decoration`, not generated state.
- **Runtime plumbing still missing**: emitting `TextChangeEvent` for key-driven edits (the scene
  already routes `EditText` to the focused node and `text_change_event(event)` builds the payload),
  and the generated payload adapter. When that lands the demo drops `publish_editor_state` and
  asserts the painted field follows `onChange` + `apply_state()` with no demo glue.

Remaining demo-documented gaps: the sample emits no `automationId`, so nodes are resolved by
element kind; inline style attachment is still missing, so the demo supplies the style-sheet
values; paint emits no caret for an empty text run (empty text paints no decoration), so an empty
focused field has no visible caret until the paint pass grows an empty-run caret.


Deferred: dynamic list elements and list-typed props (`Component::props` is not populated yet, so a
list can be seeded only from a literal array initial or another list reference), virtualized lists
(`VirtualList`/`VirtualGrid`),
component composition and `Slot`, reusing computed styles,
multi-file watch mode, cross-module incremental generation, and single-node incremental
regeneration. Goldens live in
`tests/golden/codegen/`; update them deliberately with `codegen_tests --write` (then rebuild: the
build compiles the generated sample as `tessera_codegen_sample`, so it is never hand-maintained).

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

- The open questions listed in [`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 18 (the built-in
  hook manifest format is specified in its section 7.1).
- Parser error recovery for the live view (editing partially valid files).
- Source maps from generated C++ back to TSX.
- Incremental codegen at the node level vs component level.
- How much static analysis for effect dependencies.
- Error recovery for partially valid files (V1: fail fast).
- Migration tooling for binding renames.
