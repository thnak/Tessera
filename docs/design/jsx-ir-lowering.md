# Lowering the JSX AST to IR (slice 5 design)

Status: design only. No lowering code exists. `src/compiler/ir/`, `src/compiler/codegen/` and
`src/compiler/rules/` are empty folders. The input is the parser of
[`jsx-frontend.md`](jsx-frontend.md); the output is the IR of [`ir-v1.md`](../specs/ir-v1.md)
([ADR-007](../../decisions/ADR-007-compiler-ir.md)). The accepted source forms are in
[`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) (the "subset spec"); this note links to its
sections instead of copying them. Questions for the project owner are numbered Q1 to Q16 at the end.

## 1. Pipeline and ownership

```text
parse_module -> analysis (Tier 1, new) -> lowering (Tier 2, new) -> IR -> IR validation -> codegen
```

- The parser (`tessera_jsx_frontend`) enforces syntax-only rules and knows nothing of IR.
- Analysis resolves names and types and checks declaration-dependent rules. It builds no IR.
- Lowering reads AST plus analysis result and builds IR; it alone sees both.
- Codegen and the IR validator see only IR (layering test, section 8).

## 2. Construct mapping

| AST construct | IR | Notes |
|---|---|---|
| Component item | `Component` | `name`, `source` = whole function span, `id` = `comp::<Name>`; `return <Root/>` is `root` |
| Props parameter, destructured defaults | `props[]` (`name`, `type`, `default`) | `props.x` reads become `ref` to the prop (Q8) |
| `const [x, setX] = useState<T>(e)` | `state[]` entry `state::x` | setter name is a symbol, not an IR object; `initial` is a literal or Expr (Q9) |
| `useRef<E>(null)` + `ref={r}` | `refs[]` (`ref::r`, `target` = node id) | target filled when the element is lowered |
| `useAnimatedValue(e)` | `animatedValues[]` | |
| `useEffect(fn, [deps])` | `effects[]` (`effect::<id>`, `deps`, `handler`, optional `cleanup`) | deps map to state/prop/animated ids |
| Library hooks (`useTheme`, ...) | `environment[]` | needs the hook manifest reader (subset spec 7.1); rejected until then |
| `const f = (...) => ...` with `bind::` | `Binding` kind `event`, plus a handler reference in `events` | Q4 to Q6 |
| Inline arrow of setter calls (9.2) | handler action list | IR has no node for it today (Q5) |
| Local `const x = PureExpr` | inlined `Expr`, or `local` ref | Q8 |
| Top-level constant | folded to `literal` | evaluated at compile time (subset spec 4) |
| JSX element, intrinsic tag | `Node` with `kind` from subset spec 11 | `source` = element span |
| JSX element, project/library component | reference to another component | Q7 |
| `<CustomElement element="X">` | `CustomElement` leaf | resolved against `ui/elements.json`; see [`custom-elements-v1.md`](../specs/custom-elements-v1.md) |
| Attribute string/`{expr}` | `props` entry (literal or Expr) | `key` goes to `key`, `ref` to `refs`, `className` to `props.className` |
| `style={{...}}` | `style` | key and value form: Q10 |
| `onX={handler}` | `events` | names checked against element specs |
| `{...props}` / `{...{a:1}}` | expanded in attribute order | later attribute wins (subset spec 10.2) |
| JSX text, `{expr}` text child | `Text.text` template | whitespace and entities already normalized by the parser; text in other nodes: Q11 |
| `{c && <X/>}`, `{c ? <X/> : null}` | `condition` on the node | |
| `{c ? <X/> : <Y/>}` | two nodes, `c` and `!c` | `c` is duplicated; it is pure so this is safe |
| Fragment child | children spliced into the parent | |
| `{list.map((r, i) => <R key=.../>)}` | node with `loop` | node/loop shape: Q12 |
| Expression operators | `binary`, `unary`, `conditional`, `template`, `call` | `===`/`!==` become `==`/`!=` (subset spec 8.2) |

Rejected at lowering (`TESSERA_IR_*`) because they parse but have no V1 IR form: a library hook
with no manifest entry, a `call` off the allowlist ([`ir-v1.md`](../specs/ir-v1.md) section 4.2),
member access on a value without a known record type, an unknown intrinsic prop or event, and any
node kind outside the closed list of IR section 3. The parser already rejects classes, `let`,
async, lowercase tags, `==` and the array methods.
Only items with `valid == true` are lowered; an invalid item produces no IR and no cascade.

## 3. The missing analysis pass

The parser deferred these checks (they need declarations). They run before lowering, which then
assumes a well-typed, name-resolved component instead of duplicating them.

| Check | Needs | Code (subset spec) |
|---|---|---|
| Hook rules: top-level, unconditional, fixed order, forms of section 7 | statement order | `TESSERA_REACT_HOOK_ORDER`, `UNKNOWN_HOOK`, `CUSTOM_HOOK` (parser has the last) |
| `useState` type or initial type, `<T>` omission | type table | `TESSERA_JSX_TYPE_MISMATCH` |
| Effect deps exact match, deps are names | symbol table, reads inside the effect body | per [`component-model-v1.md`](../specs/component-model-v1.md) section 2 |
| `return` placement in arrows | arrow role (handler, effect, cleanup) | `TESSERA_REACT_EFFECT_RETURN` |
| Handler form classification (setter ref, inline, native) | statements, bind pairing | section 9 |
| Event handler parameter types | per-element event table | `TESSERA_JSX_TYPE_MISMATCH` |
| `ref` target type matches the tag | refs, intrinsic table | section 10.2 |
| `key` only on the list item root | element position | section 10.2 |
| List nesting depth at most 2 | `.map` nesting | [`ir-v1.md`](../specs/ir-v1.md) section 5 |
| Expression typing, boolean conditions, no string/number mixing | types of all names | `TESSERA_JSX_TYPE_MISMATCH` |
| Props of project/library components against their prop types | other files' components | section 10.2 |
| Import resolution and cycles | project file set | `TESSERA_JSX_SUBSET_IMPORT_CYCLE` |

Decision recommended: analysis is a separate library, `tessera_react_rules` in the existing empty
`src/compiler/rules/`. It consumes AST and produces an `AnalyzedModule`: a symbol table per
component, the type of every expression node (side table keyed by node pointer), the role of every
arrow, and diagnostics. Lowering takes `(Module, AnalyzedModule)`. Reasons: the live view reparses
per component, so analysis should be incremental per component; the same checks must not exist in
two places; and lowering stays a tree-to-tree translation that can assert instead of re-validate.

## 4. Spans and diagnostics

- The AST keeps `Span` (byte offset and length). Lowering needs the file's `SourceMap` and the
  project-relative path to build the IR `SourceSpan` (`file`, `offset`, `length`, `line`, `column`,
  `endLine`, `endColumn`; [`ir-v1.md`](../specs/ir-v1.md) section 8). `SourceMap` already counts
  columns in code points and ignores a BOM for columns; confirm in a test that offsets still count
  the BOM bytes (Q16).
- `SourceSpan` is a plain IR struct (no parser headers); lowering converts.
- A node's span is its JSX element span; a component's span is the function declaration span; a
  binding's span is the annotation comment span (`Statement::bind_span`); an effect's span is its
  `useEffect` statement.
- Lowering diagnostics reuse `jsx::Diagnostic` (code, message, span, suggestion) so one report
  prints parser, analysis and lowering errors alike. Prefixes: `TESSERA_JSX_*` and `TESSERA_REACT_*`
  for source-level rules (analysis), `TESSERA_IR_*` for lowering and IR validation failures
  ([`compiler.md`](compiler.md) section 10). A component with any error produces no IR.

## 5. `bind::` ids and comments

- The parser pairs each annotation with a statement (`Statement::bind_id`, `bind_span`). Lowering
  does not look at comments again. All other comments are dropped; the IR has no comment field.
- Each paired handler or effect creates one `Binding`: `id` = `bind::<id>`, `component`, `kind`
  (`event` or `effect`), `source` = annotation span, `region` = `<id>` ([`ir-v1.md`](../specs/ir-v1.md)
  section 6). An effect with a cleanup creates a second binding `bind::<id>_cleanup`, region
  `<id>_cleanup`, kind `effect`, with the cleanup arrow's span.
- `bindings` is sorted by id. Uniqueness across the project is the IR validator's job (the parser
  checks only within one file).
- Codegen emits one protected region per binding, with the exact markers of
  [ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md), and the generated `region`
  string is the only link to developer C++. The merger (`src/compiler/merger`) therefore never
  sees AST or IR; it sees generated text.
- Idempotency, `merge(merge(g,e),e) == merge(g,e)`, depends on the generated text being a pure
  function of the IR. Lowering contributes by being deterministic (section 7) and by never putting
  anything position-dependent (line numbers, offsets) into region ids or stub text. Line numbers
  appear only in the sidecar source map.
- IR validation implements "binding annotation without a matching IR use, or vice versa"
  ([`ir-v1.md`](../specs/ir-v1.md) section 9) by comparing `bindings` with the handler and effect references.

## 6. Layout and CMake targets

```text
src/compiler/ir/            tessera_ir          IR data model, SourceSpan, JSON writer, validator
src/compiler/rules/         tessera_react_rules analysis pass (section 3)
src/compiler/lowering/      tessera_lowering    Module + AnalyzedModule -> IR
src/compiler/codegen/       (later)             depends on tessera_ir only
```

- `tessera_ir`: static library, C++23, links nothing from the project. Public headers under
  `include/tessera/compiler/ir/`: `ir.hpp` (aggregate structs for components, nodes, expressions,
  bindings, types), `source_span.hpp`, `json_writer.hpp`, `validate.hpp`. One primary type per header
  once `ir.hpp` passes 300 lines. Children are `std::unique_ptr`; no RTTI.
- `tessera_react_rules` links the front end; `tessera_lowering` links front end, rules and `tessera_ir`.
- `add_subdirectory` lines go next to the existing compiler entries in the top-level
  `CMakeLists.txt`. Tests register in `tests/unit/CMakeLists.txt` as `ir_*`, `rules_*`, `lowering_*`.
- IR in memory is the contract with codegen; JSON is the snapshot format (ADR-007). A reader waits (Q15).

## 7. Serialisation and determinism

- A hand-written writer in `tessera_ir`, no JSON dependency (a new dependency needs entries in
  `docs/specs/dependencies.md` and `THIRD_PARTY_NOTICES.md`). Output: keys in sorted order at every
  level, two-space indent, LF, one trailing newline, integers in base 10, floats as the shortest
  round-trip text (`std::to_chars`, as in [`ir-v1.md`](../specs/ir-v1.md) section 4.1), strings with the
  minimal JSON escapes and `\u00XX` for control characters, no locale.
- Ordering follows [`ir-v1.md`](../specs/ir-v1.md) section 11: components by path then name, nodes in
  source order, bindings by id. Lowering uses ordered containers or sorts explicitly; it never
  iterates a hash map to emit output.
- Debug ids: `node::N` is a pre-order counter per component starting at 1 (Q2). `uiId`/`componentId` are
  computed by one function behind an interface so tests can inject a fixed salt (Q1).

## 8. Testing plan

- **IR layer alone (no parser):** hand-built IR for the spec examples serialises to the golden
  text; the Counter example of [`ir-v1.md`](../specs/ir-v1.md) section 13 is a golden with fixed ids.
  Validator tests, one fixture per hard error of section 9.
- **Analysis:** one passing and one failing fixture per row of section 3, checked for code, span
  and message (diagnostic text golden, as `jsx_*` tests do).
- **Lowering:** TSX fixture in, IR JSON golden out (`tests/golden/ir/*.json`, `*.tsx` beside it), one
  fixture per row of the section 2 table. Golden update is a deliberate script, never automatic.
- **Spans:** slicing the source with each node's `offset`/`length` gives its element text (CRLF, non-ASCII).
- **Determinism:** lower twice and on eight threads (as `jsx_concurrent`), compare bytes.
- **Layering:** a test fails if `src/compiler/ir` or `codegen` includes `tessera/compiler/jsx/`, or
  if `tessera_ir` links a project target.
- **Invalid items:** a broken component does not change the IR of the others.
- **Mutation checks:** every new test is run against deliberate mutations of the code it covers
  (flip a comparison, drop a branch, swap sort keys) and must fail with a readable
  message; survivors are fixed or documented as equivalent. Golden tests are also mutated by
  changing the golden file by one character, which must fail.

## 9. Slice breakdown for lowering

Each slice is behavior, tests and docs, committed on its own.

1. **IR data model and writer.** `tessera_ir`: types, `SourceSpan`, JSON writer, the Counter golden.
   No parser dependency.
2. **IR validator.** Section 9 hard errors on hand-built IR; the layering test.
3. **Analysis skeleton.** `tessera_react_rules`: symbol table per component, hook forms and order,
   arrow roles (handler/effect/cleanup), `return` placement, effect dependency matching.
4. **Types.** Type table (props, aliases, records), expression typing, boolean conditions, event
   payload types, `ref` targets, `key` placement, list depth.
5. **Lowering of expressions.** `Expr` -> IR `Expr` with the operator mapping and folding of
   top-level constants; no components yet.
6. **Lowering of hooks and bindings.** state, refs, animated values, effects, cleanup, bindings and
   their spans.
7. **Lowering of nodes.** Intrinsic kinds, attributes, text, children, fragments, events, `style`,
   `className`, `ref` targets.
8. **Conditionals and loops.**
9. **Components across files.** Imports, references, cycles, prop checks; `LoweringOptions`.
10. **Identity.** `uiId`, `componentId`, automation manifest, collision errors (after Q1).
11. **Library and custom elements.** Hook manifest reader, `environment`, `ui/elements.json`.

Slices 1 and 2 need no decision beyond Q2 and can start first. Slices 5 to 8 wait for Q4 to Q7 and
Q12, which change IR shape.

## 10. Questions for the project owner

Each has a recommended answer. IR-shape answers need an additive edit of [`ir-v1.md`](../specs/ir-v1.md)
(optional fields keep `irVersion` 1, section 12).

1. **ID hash and salt.** `hash64` and the source of `project_salt` are unspecified
   ([`automation-ids-v1.md`](../specs/automation-ids-v1.md) section 2 and 9). Recommend FNV-1a 64-bit,
   salt = `app.namespace`, tests inject a fixed salt; slice 10 only.
2. **Debug node ids.** Recommend `node::N`, pre-order per component from 1, as the examples show.
3. **Same component name in two files.** Ids differ by path, but `comp::<Name>` would collide;
   recommend a duplicate name across files is an error.
4. **Un-annotated `const f = arrow` handler.** Subset spec 9.3 defines native handlers by the
   annotation; 9.2 defines inline handlers only inline. Recommend: an un-annotated declared arrow
   whose body is setter calls only is a reusable inline handler; any other body is an error
   `TESSERA_REACT_BIND_MISSING` (new code).
5. **Where inline setter handlers live in IR.** IR has only `events: name -> binding id`. Recommend
   an optional component `handlers[]` of `{id: "handler::N", params, actions: [set(target, value) |
   animSet(target, value, animate)], source}`, and `events` values may be `handler::N`.
6. **Native handler signature and preview body.** The C++ stub needs typed parameters and the
   designer needs the preview body, but `Binding` has neither. Recommend optional `params`,
   `returnType` and `preview` (the action list plus optional `return` Expr) on the binding.
7. **Component references.** IR section 3 says a custom component reference "is lowered to its root
   node", which loses per-instance state if it means inlining. Recommend a node form that references
   the component with its prop expressions and is not inlined, e.g. kind `Component` with
   `component` and `props`; this adds to the closed kind list.
8. **Prop, local and loop refs.** Only `state::x` is shown. Recommend `prop::x`, `local::x`
   (locals are inlined unless used twice; recommend always inlining), `item`/`index` names as written,
   and dotted ids for record fields (`item.id`, `prop::row.label`).
9. **State `initial` and prop `default`.** The spec allows pure expressions (a prop as the initial
   value). Recommend the IR field holds an Expr, written as a bare JSON value when it is a literal.
10. **`style` keys and values.** Subset spec 10.3 converts keys to kebab-case CSS names; the IR example
    shows `fontSize`. Recommend kebab-case (the normative text), numbers converted to `"<n>px"` for
    length properties using the property table of [`stylesheet-v1.md`](../specs/stylesheet-v1.md)
    section 11, dynamic values as Expr. This needs the compiler to read that table; recommend a
    generated header shared with the runtime registry rather than linking the UI library.
11. **Text under `View`/`Button`.** Subset spec 10.4 is silent for non-`Text` parents. Recommend a
    `Button` with only text children gets `props.label`; otherwise text becomes a synthetic `Text` child
    whose span is the text run.
12. **Loop shape.** IR section 3 puts `loop` on a node; section 5 shows a `loop` with its own `body`.
    Recommend the node carrying `loop` is the repeated template and `loop` has no `body`; the node's
    `key` holds the key expression.
13. **Multi-file input.** Slice 9 needs all files of a project, but the front end is file-at-a-time.
    Recommend `lower_project(files, options)` in `tessera_lowering`, with file reading in the caller.
14. **`module.name`.** Recommend it comes from `LoweringOptions` (project config `app.namespace`),
    not from a file name.
15. **JSON reader.** Recommend writing the reader only when the Wasm evaluator or codegen needs it, with a
    round-trip test; until then IR is consumed in memory.
16. **Offsets with a BOM.** The IR says byte offset in the file. Recommend offsets include the BOM
    bytes and columns exclude it (what `SourceMap` does); the spec should say so.

## 11. What the repo does not show

- The exact set of events and props per intrinsic: they live in other specs
  ([`component-model-v1.md`](../specs/component-model-v1.md), text-editing, scroll) and no generated
  table exists yet. Slice 4 needs a machine-readable intrinsic table; its format and owner are open.
- Whether the compiler can read `ui.hooks.*` config yet (no config loader under `src/compiler`).
