# Lowering the JSX AST to IR (slice 5 design)

Status: slices 1 to 3 are implemented (IR model and writer, IR validator, analysis skeleton, see
"Slice 3 status" below). No lowering code exists and `src/compiler/codegen/` is an empty folder. The
input is the parser of
[`jsx-frontend.md`](jsx-frontend.md); the output is the IR of [`ir-v1.md`](../specs/ir-v1.md)
([ADR-007](../../decisions/ADR-007-compiler-ir.md)). The accepted source forms are in
[`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) (the "subset spec"); this note links to its
sections instead of copying them. Decisions of the project owner are numbered 1 to 16 in section 10.

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
| Props parameter, destructured defaults | `props[]` (`name`, `type`, `default`) | `props.x` reads become `ref` to the prop (decision 8) |
| `const [x, setX] = useState<T>(e)` | `state[]` entry `state::x` | setter name is a symbol, not an IR object; `initial` is a literal or Expr (decision 9) |
| `useRef<E>(null)` + `ref={r}` | `refs[]` (`ref::r`, `target` = node id) | target filled when the element is lowered |
| `useAnimatedValue(e)` | `animatedValues[]` | |
| `useEffect(fn, [deps])` | `effects[]` (`effect::<id>`, `deps`, `handler`, optional `cleanup`) | deps map to state/prop/animated ids |
| Library hooks (`useTheme`, ...) | `environment[]` | needs the hook manifest reader (subset spec 7.1); rejected until then |
| `const f = (...) => ...` with `bind::` | `Binding` kind `event`, plus a reference in `events` | decisions 4 to 6 |
| Inline arrow of setter calls (9.2) | `handlers[]` record (`handler::N`) | decision 5 |
| Local `const x = PureExpr` | inlined `Expr` (decision 8) | |
| Top-level constant | folded to `literal` | evaluated at compile time (subset spec 4) |
| JSX element, intrinsic tag | `Node` with `kind` from subset spec 11 | `source` = element span |
| JSX element, project/library component | `Component` node (decision 7) | not inlined |
| `<CustomElement element="X">` | `CustomElement` leaf | resolved against `ui/elements.json`; see [`custom-elements-v1.md`](../specs/custom-elements-v1.md) |
| Attribute string/`{expr}` | `props` entry (literal or Expr) | `key` goes to `key`, `ref` to `refs`, `className` to `props.className` |
| `style={{...}}` | `style` | key and value form: decision 10 |
| `onX={handler}` | `events` | names checked against element specs |
| `{...props}` / `{...{a:1}}` | expanded in attribute order | later attribute wins (subset spec 10.2) |
| JSX text, `{expr}` text child | `Text.text` template | whitespace and entities already normalized by the parser; text in other nodes: decision 11 |
| `{c && <X/>}`, `{c ? <X/> : null}` | `condition` on the node | |
| `{c ? <X/> : <Y/>}` | two nodes, `c` and `!c` | `c` is duplicated; it is pure so this is safe |
| Fragment child | children spliced into the parent | |
| `{list.map((r, i) => <R key=.../>)}` | node with `loop` | node/loop shape: decision 12 |
| Expression operators | `binary`, `unary`, `conditional`, `template`, `call` | `===`/`!==` become `==`/`!=` (subset spec 8.2) |

Rejected at lowering (`TESSERA_IR_*`) because they parse but have no V1 IR form: a library hook
with no manifest entry, a `call` off the allowlist ([`ir-v1.md`](../specs/ir-v1.md) section 4.2),
member access on a value without a known record type, an unknown intrinsic prop or event, and any
node kind outside the closed list of IR section 3. The parser already rejects classes, `let`,
async, `==` and the array methods. Lowercase tags are HTML aliases resolved in analysis
([ADR-039](../../decisions/ADR-039-lowercase-html-tags-accepted.md)); the parser stops rejecting them
when that slice lands.
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

### Slice 3 status (analysis skeleton)

`tessera_react_rules` (`src/compiler/rules/`, headers `include/tessera/compiler/rules/`, namespace
`tessera::compiler::rules`) links `tessera_jsx_frontend` only; a test checks that its sources include no
`tessera/compiler/ir/` header and that CMake does not link `tessera_ir`. `analyze_module(Module, SourceMap)`
returns an `AnalyzedModule`: per component a `ComponentAnalysis` (symbol table of props, state, setters,
refs, animated values, locals and handlers; `effects`; the role of every arrow) plus all diagnostics, in
component then statement order. It points into the `Module`, which must outlive it. A component with any
error has `ok == false`; items the parser marked invalid are listed with `ok == false` and not analysed.
All state is per call.

| Check | Code | Notes |
|---|---|---|
| Hook not at the top level of the body (condition, `&&`/`\|\|` right side, function, JSX, hook argument) | `TESSERA_REACT_HOOK_ORDER` | one report per call |
| Hook declared after a statement that reads what it declares | `TESSERA_REACT_HOOK_ORDER` | the fixed-order rule (reading of "fixed order" is a slice question) |
| `use*` call that is not a core hook | `TESSERA_REACT_UNKNOWN_HOOK` | `useReducer`, `useContext` and unsafe forms of `useMemo`/`useCallback` give `TESSERA_REACT_UNSUPPORTED_API`; the safe forms lower as locals/handlers (slice 13) |
| Hook not in its section 7 form | `TESSERA_REACT_HOOK_FORM` (new) | `useState` pair, `useRef` initial `null`, single-name `useAnimatedValue`, `useEffect` arrow plus array |
| `return` in an effect body other than the final cleanup arrow | `TESSERA_REACT_EFFECT_RETURN` | cleanup arrows (including their own bodies) are checked too |
| Dependencies not exact | `TESSERA_REACT_EFFECT_DEPS` | missing, extra, duplicate, non-name, non-array, no array, not a prop/state/animated value |
| `const f = arrow` without `bind::`, body not setter calls only | `TESSERA_REACT_BIND_MISSING` (new) | decision 4; an empty body also fails |

Arrow roles: `NativeHandler` (with bind id), `InlineHandler`, `Effect`, `Cleanup` (bind id plus
`_cleanup`), `EventArrow` (arrow written as an attribute value), `ListCallback` (`.map` argument), `Other`.
Dependency matching reads the effect body for props, state and animated values; setters, `v.set(...)` and
refs are not reads; a read of a local counts as a read of what the local reads (locals are inlined,
decision 8); arrow parameters shadow outer names; `props.x` of a non-destructured props parameter is
reported as `props.x` because only names can be dependencies.

Not in slice 3: types and expression typing, `ref` targets, `key` placement, list depth (slice 4),
`SETTER_IN_RENDER`, `HOOK_NOT_ALLOWED` and manifest hooks, duplicate declarations, imports (slice 9).
Questions raised by the slice are listed in the slice report; the choices made here are the
least-committal ones.

### Slice 5 status (expression lowering)

`tessera_lowering` (`src/compiler/lowering/`, public headers
`include/tessera/compiler/lowering/`, namespace `tessera::compiler::lowering`) links
`tessera_jsx_frontend` and `tessera_ir` and nothing else. A configure check fails if that list
changes, and `lowering_layering` checks that lowering includes only those headers while the IR
still includes no front-end header. The API is:

```cpp
struct ExpressionLoweringContext {
    const jsx::SourceMap& source_map;
    std::string_view file;               // project-relative, POSIX separators
    const NameResolver* names = nullptr; // null rejects every reference
};
std::expected<ir::ExprPtr, std::vector<jsx::Diagnostic>>
lower_expression(const jsx::Expr& expr, const ExpressionLoweringContext& context);
```

`to_source_span(map, span, file)` in `lowering/source_span.hpp` implements section 4 and decision 16
(offsets are bytes, columns are code points, a BOM counts in the offset and not in the column).

What the slice lowers:

| Source (`jsx::Expr`) | IR (`ir::Expr`) | Notes |
|---|---|---|
| `Number`, `String`, `Boolean`, `Null` | `Literal` | string text is already decoded; `null` is `ir::Null` |
| `Identifier`, `Member` chain | `Ref` | the chain is passed verbatim to `NameResolver` |
| `Unary` `! - +` | `Unary` | any other unary operator is rejected |
| `Binary` | `Binary` | `===`→`==`, `!==`→`!=`; the rest of the IR set is copied |
| `Conditional` | `Conditional` | operands are test, then, else |
| `Template` | `Template` | text and substitution parts alternate; empty text parts are kept |
| `Call` | `Call` | callee must be a bare name from IR section 4.2 |

The reference seam is `NameResolver`
(`include/tessera/compiler/lowering/name_resolver.hpp`): `resolve("props.title")` returns
`"prop::title"` or `std::nullopt`, and a null resolver in the context rejects every reference. The
decision is deliberately the least committal one: slice 3 exposes per-component symbol tables but
no record field types, and slice 4 has not landed, so lowering does not try to infer whether
`row.title` is `prop::row.title`, `item.title` or `state::row.title`. The analysis/type passes
will implement the resolver in slice 9; tests supply a small map. The returned id is used
verbatim and is checked against the component scope later by the IR validator.

Numeric literal typing is silent in the spec. The slice 5 rule is: a spelling with `.` or an
exponent is a float (`0.5`, `1e3`, `1.0`); an integer spelling that fits signed 32-bit is an int
(`0`, `42`, `2147483647`); an integer spelling outside int32 stays a float because the IR has no
wider int. The last case is provisional: slice 4 may reject or retype it once literal types are
checked. A non-finite number is `TESSERA_IR_LITERAL`.

Diagnostics reuse `jsx::Diagnostic` with `TESSERA_IR_*` codes: `TESSERA_IR_UNSUPPORTED` for array
and object literals, arrows, JSX in an expression, assignment, indexing, member access on a value
without a record type, unsupported unary/binary operators and non-allowlisted calls;
`TESSERA_IR_UNRESOLVED` for a reference without a resolver or one the resolver rejects;
`TESSERA_IR_MALFORMED` for a hand-built node with the wrong operand count or missing operands; and
`TESSERA_IR_LITERAL` for a non-finite number. Lowering stops at the first error, so the result is
either a complete expression or diagnostics with no IR at all.

Not in slice 5: types and expression typing (slice 4); hooks, bindings, nodes, components,
conditionals/loops in JSX and imports (slices 6 to 9); array and object literals in their accepted
positions; an implementation of `NameResolver` from the analysis result; and IR expressions carry
no source span in v1, so spans appear only on diagnostics (`to_source_span` is ready for the nodes
and bindings of slices 6 to 8). Top-level constant folding (section 2, expression row) is **not**
implemented: it needs the module (constant declarations, imports, their values) and the type
table, none of which exist yet, so no folding semantics were invented here.

Tests are `lowering_expression`, `lowering_reject`, `lowering_span`, `lowering_determinism`,
`lowering_golden` and `lowering_layering`, registered in `tests/unit/CMakeLists.txt`. The golden
`tests/golden/lowering/expressions.json` is updated deliberately with
`lowering_golden_tests --write tests/golden/lowering/expressions.json`, never automatically.

## 4. Spans and diagnostics

- The AST keeps `Span` (byte offset and length). Lowering needs the file's `SourceMap` and the
  project-relative path to build the IR `SourceSpan` (`file`, `offset`, `length`, `line`, `column`,
  `endLine`, `endColumn`; [`ir-v1.md`](../specs/ir-v1.md) section 8). `SourceMap` already counts
  columns in code points and ignores a BOM for columns; confirm in a test that offsets still count
  the BOM bytes (decision 16).
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
- IR in memory is the contract with codegen; JSON is the snapshot format (ADR-007). A reader waits (decision 15).

### Validator coverage (slice 2)

`validate(const IrModule&)` in `tessera_ir` returns `TESSERA_IR_*` diagnostics: version; duplicate
and missing ids (components, nodes, `uiId`, `componentId`, handlers, bindings, declarations);
`automationId` equal to `uiId`; unresolved references, events, effect dependencies, effect
bindings, ref targets, action targets and `Component` targets; operators and functions outside
[`ir-v1.md`](../specs/ir-v1.md) section 4; operand counts; type syntax; spans; leaf nodes,
`ErrorBoundary` shape, loop key, loop source and loop depth 2; binding id, region and unused
bindings; sorted components and bindings.

Not covered: type checking of expressions and props (the analysis pass), element and prop schemas
of `CustomElement` and intrinsics, a `Component` node's props against the referenced component's
props, 64-bit id hashing, warnings (unused state or refs), and record type declarations (any
identifier is accepted as a record type name until the type table exists).

## 7. Serialisation and determinism

- A hand-written writer in `tessera_ir`, no JSON dependency (a new dependency needs entries in
  `docs/specs/dependencies.md` and `THIRD_PARTY_NOTICES.md`). Output: keys in sorted order at every
  level, two-space indent, LF, one trailing newline, integers in base 10, floats as the shortest
  round-trip text (`std::to_chars`, as in [`ir-v1.md`](../specs/ir-v1.md) section 4.1), strings with the
  minimal JSON escapes and `\u00XX` for control characters, no locale.
- Ordering follows [`ir-v1.md`](../specs/ir-v1.md) section 11: components by path then name, nodes in
  source order, bindings by id. Lowering uses ordered containers or sorts explicitly; it never
  iterates a hash map to emit output.
- Debug ids: `node::N` is a pre-order counter per component starting at 1 (decision 2). `uiId`/`componentId` are
  computed by one function behind an interface so tests can inject a fixed salt (decision 1).

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
3. **Analysis skeleton (done).** `tessera_react_rules`: symbol table per component, hook forms and order,
   arrow roles (handler/effect/cleanup), `return` placement, effect dependency matching.
4. **Types.** Type table (props, aliases, records), expression typing, boolean conditions, event
   payload types, `ref` targets, `key` placement, list depth.
5. **Lowering of expressions (done).** `Expr` -> IR `Expr` with the operator mapping and folding of
   top-level constants; no components yet.
6. **Lowering of hooks and bindings.** state, refs, animated values, effects, cleanup, bindings and
   their spans.
7. **Lowering of nodes.** Intrinsic kinds, attributes, text, children, fragments, events, `style`,
   `className`, `ref` targets.
8. **Conditionals and loops.**
9. **Components across files.** Imports, references, cycles, prop checks; `LoweringOptions`.
10. **Identity.** `uiId`, `componentId`, automation manifest, collision errors (after decision 1).
11. **Library and custom elements.** Hook manifest reader, `environment`, `ui/elements.json`.
12. **Schema table.** `tessera_schema` (`src/schema/`): the `constexpr` intrinsic, alias and style
    tables, the dump tool (JSON and `.d.ts`) and its golden
    ([`machine-readable.md`](../specs/jsx-intrinsics-v1/machine-readable.md)). Slice 4 (types) needs
    the intrinsic part of it, so it comes before slice 4 or is built with it.
13. **Lowercase alias and `react` import.** Analysis-side alias resolution and attribute rewrites,
    parser change, `from "react"` hooks alias, `useMemo`/`useCallback` safe-form lowering
    ([ADR-039](../../decisions/ADR-039-lowercase-html-tags-accepted.md);
    [`react-compat.md`](../specs/jsx-intrinsics-v1/react-compat.md) sections 4.2 and 5). Needs slice 12.

Slices 1 and 2 can start first.

## 10. Decisions

The project owner accepted every recommended answer to the sixteen questions of the first version
of this note. Numbers are kept; text elsewhere cites them as "decision N". IR-shape decisions
(5, 6, 7, 9, 12, 16) are recorded in [`ir-v1.md`](../specs/ir-v1.md) as optional fields, so
`irVersion` stays 1; the `Component` node kind is the one addition to the closed kind list.

1. **ID hash and salt.** FNV-1a 64-bit; `project_salt` is `app.namespace`; tests inject a fixed
   salt. Implemented in slice 10 only.
2. **Debug node ids.** `node::N`, a pre-order counter per component starting at 1.
3. **Same component name in two files** is an error.
4. **Un-annotated declared arrow.** A `const f = arrow` without `bind::` whose body is setter calls
   only is a reusable inline handler. Any other body is `TESSERA_REACT_BIND_MISSING` (new code).
5. **Inline setter handlers** are `handlers[]` records with `handler::N` ids; `events` may point
   at them ([`ir-v1.md`](../specs/ir-v1.md) section 2.1).
6. **Native handler signature and preview body** are optional `params`, `returnType` and `preview`
   on the `Binding` ([`ir-v1.md`](../specs/ir-v1.md) section 6).
7. **Component references** are not inlined: a `Component` node with `component` and `props`
   ([`ir-v1.md`](../specs/ir-v1.md) section 3.4).
8. **Refs.** `prop::x`; `local::x` is always inlined, so no `local::` ref is emitted; loop `item` and
   `index` are named as written; record fields are dotted ids (`item.id`, `prop::row.label`).
9. **State `initial` and prop `default`** are Expressions, written as the bare JSON value when
   literal.
10. **`style`.** Keys are kebab-case CSS names; numbers become `"<n>px"` for length properties,
    using a table generated from one source shared with the runtime property registry (the compiler
    does not link the UI library); dynamic values are Expressions.
11. **Text under `View`/`Button`.** A `Button` whose children are only text gets `props.label`;
    otherwise each text run becomes a synthetic `Text` child spanning the run.
12. **Loop shape.** The node carrying `loop` is the repeated template; `loop` has no `body`; the
    node's `key` holds the key Expression.
13. **Multi-file input.** `lower_project(files, options)` in `tessera_lowering`; file reading stays
    with the caller.
14. **`module.name`** comes from `LoweringOptions` (project `app.namespace`).
15. **JSON reader** is deferred until the Wasm evaluator or codegen needs one.
16. **BOM.** Offsets count the BOM bytes; columns do not.

## 11. What the repo does not show

- The exact set of events and props per intrinsic is now in
  [`jsx-intrinsics-v1`](../specs/jsx-intrinsics-v1/README.md) (its machine-readable form is a proposal
  there). It used to live in other specs
  ([`component-model-v1.md`](../specs/component-model-v1.md), text-editing, scroll) and no generated
  table exists yet. Slice 4 needs a machine-readable intrinsic table; its format and owner are open.
- Whether the compiler can read `ui.hooks.*` config yet (no config loader under `src/compiler`).
