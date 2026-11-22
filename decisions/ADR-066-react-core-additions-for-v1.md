# ADR-066: React core additions for V1

- Status: Proposed (2026-11-20). Owner direction: "agreed" to the V1 proposals of
  [`react-core-coverage.md`](../docs/reference/react-core-coverage.md). That table is the detailed
  record; this ADR is the decision that fixes the set, the order and the IR changes it needs. The L
  rows are in [ADR-067](ADR-067-pending-ui-transitions-suspense-lazy.md), custom hooks in
  [ADR-065](ADR-065-custom-hooks-by-compile-time-inlining.md).
  **Stays Proposed on purpose** (owner direction 2026-11-22): the set and the order are expected to
  move as V1 ports real code, so locking them now would make every later correction an amendment.
  The steps #651 to #654 therefore land while this ADR is Proposed, under the "Proposed on purpose"
  rule of [`AGENTS.md`](../AGENTS.md#farming-protocol); each landing amends this ADR with what it
  measured, and the owner moves it to Accepted when the set has stopped moving.
- Date: 2026-11-20
- Related: [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) (amended: items on rejected hooks
  and syntax), [ADR-058](ADR-058-react-vite-port-compatibility-tiers.md) (typing rules and ports),
  [ADR-051](ADR-051-ir-record-types-and-record-values.md) (records),
  [`jsx-subset-v1.md`](../docs/specs/jsx-subset-v1.md), [`ir-v1.md`](../docs/specs/ir-v1.md),
  [`react-compat.md`](../docs/specs/jsx-intrinsics-v1/react-compat.md).
- Terms: an **addition** is a React form the V1 front end accepts after this ADR. A **cheap acceptance**
  changes only the parser or a rule; a **lowering addition** changes how a form lowers to IR.

## Context

The owner's rule is that the core of React is kept. The inventory shows that most of it already is, a
set of forms is rejected for reasons that no longer hold (arrow components, `import React`, `useMemo`)
and a few need real work (handler control flow, list and string expressions, reducer and context).
Deciding the whole set now lets the farm (`AGENTS.md`, farming protocol) land it in order instead of
growing the codebase around the gaps, which makes each later change more expensive.

## Decision

1. **Cheap acceptances, in the first landing.**
   - Arrow-function components and `export default function`: same rules as a function declaration.
   - `import React from "react"` and `import * as React from "react"`: accepted and ignored; any other
     name from `react` stays an error (Q15 unchanged).
   - `memo(Component)`: accepted as identity. `StrictMode`, `Profiler`: accepted as pass-through.
   - `useId`: a stable id from the instance key and the call ordinal.
   - `useLayoutEffect`: accepted with `useEffect` timing.
   - `useMemo`, `useCallback`: the safe forms of `react-compat.md` section 4.2 lower to locals and inline
     handlers; other uses keep `TESSERA_REACT_UNSUPPORTED_API`.
   - `autoFocus`: a focus request at mount, defined with the focus behaviors of ADR-064.
   - `??` and `?.` over `optional<T>`: IR expressions with a type rule (`ir-v1-expr.md` section 4).
2. **Handler control flow.** A handler body may use `if`/`else`, `const` and early `return`, lowered to a
   conditional action in the handler IR (`ir-v1.md` section 2.1: a new `if` action with a pure `cond`
   and two action lists). `let`, loops and mutation stay in native handlers; a `switch` is accepted only
   inside a reducer (item 4).
3. **List and string expressions.** The pure-expression allowlist gains `filter` and `slice` on lists
   (with a pure arrow predicate), `.length` as `length(list)`, and the total string functions `trim`,
   `toUpperCase`, `toLowerCase`, `startsWith`, `slice`, `toFixed`, plus `parseIntOr` and `parseFloatOr`
   (Q26). `sort` is later. Object spread `{...state, x}` is accepted for record values with known fields
   (ADR-051). No function value other than an arrow predicate or a declared handler appears at run time.
4. **`useReducer`, limited.** `const [state, dispatch] = useReducer(reduce, initial)` where `reduce` is a
   pure function over a record state and a closed action union, written as one `switch` on the action
   type returning a record, lowers to one state cell and one handler per action type;
   `dispatch({ type: "x" })` is a call of that handler. Anything else is `TESSERA_REACT_REDUCER_FORM`.
5. **`useContext`, static.** `createContext<T>(default)` and `<Ctx.Provider value={...}>` lower to an IR
   `Environment` with a typed default and a compile-time provider tree: the provider's value is a pure
   expression of its component's props and state, and a consumer resolves the nearest provider
   statically. A provider that cannot be resolved at compile time is `TESSERA_REACT_CONTEXT_DYNAMIC`.
6. **Order.** (1) the parser and type-grammar gaps the spike measured — a module-scope helper
   function, a destructured local, `readonly`, a JSDoc comment inside a props type; (2) cheap
   acceptances; (3) custom hooks (ADR-065); (4) handler control flow; (5) list and string
   expressions and record spread; (6) `useReducer` and `useContext`; the L rows of ADR-067 run in
   parallel with (4) to (6) because they touch different stages. Each step lands as one PR that
   closes its issues, with its spec text, tests and diagnostics (`AGENTS.md`, landing size). The
   order is the one the spike's frequency table chose, not the one this ADR first proposed: see
   "Spike #660 evidence" below, which also moves `memo` off the list (already accepted) and
   `autoFocus` to the schema (the prop exists on `View`, not on the input elements).
7. **IR compatibility.** Items 2, 3 and 5 add optional IR kinds (an `if` action, list and string calls,
   record update, context environments); `irVersion` stays `1` under the additive rule of `ir-v1-expr.md`
   section 12. The compiler, evaluator and generator ship together.
8. **Still rejected, with the Tessera form.** Spread props, render props, `forwardRef`, callback refs,
   portals, class and async components, `useSyncExternalStore` (use `useObservable`), Fragment as the
   returned root, keyed remount through `key`, `dangerouslySetInnerHTML`, browser APIs.

## Consequences

### Positive

- A React project of ordinary style (arrow components, `import React`, list filtering, reducers,
  context) ports with small edits, as ADR-058 aims.
- The set is fixed before the farm reaches it, so the work lands in a defined order with specs and tests.
- Control flow in handlers removes the most common reason a port needs a native handler.

### Negative / risks

- The IR allowlist and the handler IR grow; the evaluator, the code generator and the preview must all
  implement each addition (the differential test of `ir-v1-expr.md` section 1 guards them).
- Each added form is a promise: accepted source must behave as the React reader expects, so every item
  needs conformance tests against the written semantics, not only a parser test.
- `useLayoutEffect`, `memo` and `StrictMode` are accepted with weaker meaning than in React; the diagnostics
  and docs must say so, or a reader will assume the React behavior.
- `useContext` and `useReducer` are restricted; code outside the restriction fails with a diagnostic that
  names the allowed form, not a silent difference.

## Alternatives considered

- **Accept only the cheap rows and keep the rest for later.** Rejected: control flow in handlers and
  list expressions are the most frequent gaps in real code; postponing them leaves ports to native
  handlers, and the codebase grows around that workaround.
- **Run a JavaScript subset in handlers.** Rejected: no JS runtime (ADR-024).
- **Add each form as a separate ADR.** Rejected: they share the IR additive rule and one order of work;
  one ADR keeps the dependencies visible.

## Evidence required before Accepted

This ADR stays Proposed until measured evidence exists, so a wrong choice is amended before it is locked. Spike [#660](https://github.com/thnak/Tessera/issues/660) measures what the front end accepts on a corpus of real components and reports the frequency of each rejection. The order of item 6 is chosen by that table and may change.

**The implementing issues no longer wait for Acceptance** (owner direction 2026-11-22, recorded in
the status line): #651 to #654 land while this ADR is Proposed, because the set is expected to move
during V1 and a locked decision would turn each correction into an amendment. Each landing appends
what it measured to this section. The owner moves the ADR to Accepted when the set stops moving.

### Spike #660 evidence (recorded 2026-11-21)

Tool: `tools/corpus/` (`tessera_corpus_measure`), test `corpus_measure`, golden
`tests/golden/corpus/react-front-end.txt`. Corpus: `tests/fixtures/corpus/react/`, 50 files written
in this repository (MIT) to mirror the common React spellings, so the measurement needs no network
and no downloaded code. A second, independent corpus is the repository's own React sources
(`designer/web/src`, `tools/wiki/src`, `tools/reference/instrument-mock/src`, 60 files); it is
measured but not committed as a golden, because it changes with every UI commit.

Two stages are measured, because a port needs both. **Front end** = parse plus the Tier-1 rules.
**Pipeline** = the whole single-file path the CLI and the preview run (parse, analyse, lower,
validate), reporting the first blocking diagnostic. The front-end table ranks the React forms; the
pipeline number is the honest "does it compile" share.

**The model holds.** The front end accepts 21 of 50 files (42.0%) and the pipeline accepts 9
(18.0%). Every accepted file declares a component. The accepted set is the one the coverage table
calls `keep`: a function component with destructured props and literal defaults, `useState` with a
typed call, a setter-only arrow handler, `useRef` with a `focus()` statement, `.map` with `key`,
`cond && <X/>` and `cond ? <A/> : <B/>`, a template string, an inline `style` object literal,
`useEffect` with an exact dependency list and a `bind::` annotation, and `memo(Component)` — which
the coverage table lists as `add`, but which already compiles because `memo` is only a call whose
result is a `const`, and the component it wraps is an ordinary declaration.

**Front-end rejection frequency** (code, occurrences, files; 50 files, 29 rejected):

| Code | Occ. | Files |
|---|---|---|
| `TESSERA_JSX_PARSE_UNEXPECTED_TOKEN` | 11 | 11 |
| `TESSERA_JSX_SUBSET_IMPORT` | 4 | 4 |
| `TESSERA_REACT_UNKNOWN_HOOK` | 4 | 4 |
| `TESSERA_JSX_SUBSET_ARRAY_METHOD` | 3 | 3 |
| `TESSERA_REACT_UNSUPPORTED_API` | 3 | 3 |
| `TESSERA_JSX_SUBSET_COMPONENT_NAME` | 2 | 2 |
| `TESSERA_JSX_PARSE_MISSING_SEMICOLON` | 1 | 1 |
| `TESSERA_JSX_SUBSET_DOM` | 1 | 1 |
| `TESSERA_JSX_SUBSET_DYNAMIC_TAG` | 1 | 1 |
| `TESSERA_JSX_SUBSET_LITERAL` | 1 | 1 |
| `TESSERA_JSX_SUBSET_MUTATION` | 1 | 1 |
| `TESSERA_JSX_SUBSET_OPERATOR` | 1 | 1 |
| `TESSERA_JSX_SUBSET_STATEMENT` | 1 | 1 |
| `TESSERA_REACT_CUSTOM_HOOK` | 1 | 1 |
| `TESSERA_REACT_EFFECT_DEPS` | 1 | 1 |
| `TESSERA_REACT_HOOK_ORDER` | 1 | 1 |

### Addendum (2026-11-22): the numbers above moved

Custom hooks landed (#645–#648, PR #759), so the measurement this section records is out of date.
The corpus golden now reads **25 of 50 accepted (50.0%)** at the front end and **12 of 50 (24.0%)**
through the pipeline, up from 24 and 11. `TESSERA_REACT_CUSTOM_HOOK` no longer appears in any
rejection table: `react/rejected/custom-hook.tsx` goes from `rejected ... pipeline=
TESSERA_REACT_CUSTOM_HOOK` to `accepted ... pipeline=ok`.

The tables below are the 2026-11-21 measurement and are kept as the record the order of work was
chosen from. The current numbers are `tests/golden/corpus/react-front-end.txt`, which the
`corpus_measure` test pins byte for byte.

This does not change the ADR's decision or its order: custom hooks were ADR-065's item, not one of
this ADR's four. It does mean the "first rejection per file" table below overstates
`REACT_CUSTOM_HOOK` by one file, and that the remaining work of items 1 to 5 is measured against a
slightly larger accepted set than the one recorded here.

**First rejection per file** — the table the order of work is actually read from, because a file
blocked by an earlier form cannot show what else it would need: `PARSE_UNEXPECTED_TOKEN` 6,
`SUBSET_IMPORT` 4, `SUBSET_ARRAY_METHOD` 3, `REACT_UNKNOWN_HOOK` 3, `SUBSET_COMPONENT_NAME` 2, and
one each for `PARSE_MISSING_SEMICOLON`, `SUBSET_DOM`, `SUBSET_DYNAMIC_TAG`, `SUBSET_LITERAL`,
`SUBSET_MUTATION`, `SUBSET_OPERATOR`, `SUBSET_STATEMENT`, `REACT_CUSTOM_HOOK`,
`REACT_EFFECT_DEPS`, `REACT_HOOK_ORDER`, `REACT_UNSUPPORTED_API`.

**Pipeline rejection frequency** (first blocking code, files): `PARSE_UNEXPECTED_TOKEN` 6,
`IR_UNSUPPORTED` 5, `IR_UNKNOWN_PROP` 4, `SUBSET_IMPORT` 4, `IR_UNRESOLVED` 3,
`SUBSET_ARRAY_METHOD` 3, `REACT_UNKNOWN_HOOK` 3, `SUBSET_COMPONENT_NAME` 2, then one each for
`PARSE_MISSING_SEMICOLON`, `SUBSET_DOM`, `SUBSET_DYNAMIC_TAG`, `SUBSET_LITERAL`, `SUBSET_MUTATION`,
`SUBSET_OPERATOR`, `SUBSET_STATEMENT`, `REACT_CUSTOM_HOOK`, `REACT_EFFECT_DEPS`,
`REACT_HOOK_ORDER`, `REACT_UNSUPPORTED_API`.

**The order of item 6 changes, and item 1 is not first.** The measurement contradicts the assumed
order in three ways:

1. **`TESSERA_JSX_PARSE_UNEXPECTED_TOKEN` is the largest single reason a file is rejected, and it is
   not a React form at all.** It is the parser's generic "unexpected token", and the fixtures show
   four distinct causes that the coverage table does not list: a destructured local
   (`const { state } = io;`), a `readonly` type modifier, a JSDoc comment inside a props type, and a
   module-scope helper function. The first three are cheap parser/type-grammar acceptances of the
   same size as item 1's rows, and they unblock more files than any single item-1 row. On the
   repository's own React sources the same code is the first rejection of 34 of 58 rejected files.
2. **`TESSERA_JSX_SUBSET_COMPONENT_NAME` is a false positive, not a missing feature.** Every
   `function` declaration is parsed as a component, so a lowercase module-scope helper
   (`function position(...)`, `function argsLabel(...)`) is rejected as a badly named component. On
   the repository's own sources 13 files declare such a helper and 16 files are rejected for this
   code. The fix is to distinguish a helper from a component, which is a parser change, not a React
   addition — and it is a prerequisite for measuring anything else in real code.
3. **`TESSERA_JSX_SUBSET_ARRAY_METHOD` (item 3) is real but not dominant.** It is the first
   rejection of 3 of 50 files here and 1 of 58 in the repository's sources. Item 3 stays, but it
   does not lead.

Item 2 (handler control flow) and item 4 (`useReducer`/`useContext`) are confirmed as real: the
`if`/early-`return` handler and the `switch` reducer are each rejected by exactly one code, and
`useReducer`/`useContext`/`useMemo`/`useCallback` all report `TESSERA_REACT_UNSUPPORTED_API` as the
coverage table says. `useId` and `useLayoutEffect` report `TESSERA_REACT_UNKNOWN_HOOK`, not
`UNSUPPORTED_API`: the message names only the four core hooks, so the diagnostic a port sees for
item 1's rows is the generic one.

**What the front end accepts that the pipeline rejects.** 12 of the 21 front-end-accepted files fail
later, and the reasons are not React forms: `TESSERA_IR_UNKNOWN_PROP` (4 — `autoFocus` on
`<TextInput>`, `onMouseEnter`/`onDoubleClick`, `type="checkbox"`, `htmlFor`), `TESSERA_IR_UNSUPPORTED`
(5 — a Fragment root, `StrictMode`, a native handler body, a string method call, a render prop),
`TESSERA_IR_UNRESOLVED` (3 — `.length`, a browser API read, a helper call). This is the most
important correction to the coverage table: several rows it marks `rejected` are **accepted by the
front end** and rejected by the schema or the lowering. `onMouse*`, `checked`/`type`, `label htmlFor`
and `autoFocus` are schema rows, not parser rows, so a port's first error for them is
`TESSERA_IR_UNKNOWN_PROP` and the work belongs to the schema, not to ADR-066 item 1. Conversely
`useId`/`useLayoutEffect` are genuinely parser-level.

**What this does not settle.** The corpus is 50 hand-written files, not a sample of published
projects: it measures the forms the coverage table names, not their real-world frequency, and a
frequency table over a corpus written from the same table is circular by construction. The
repository's own React sources are the independent check, and they are a different kind of code (a
designer UI, not a component library), so they bound the answer without settling it. Lowering and IR
validation are measured only as a first blocking code, not as a full frequency table. The
`TESSERA_JSX_PARSE_UNEXPECTED_TOKEN` causes are counted by hand from the fixtures, not by the tool,
because the code is generic and the tool cannot split it. A corpus of permissively licensed
third-party components, kept out of the repository and recorded only as measurements, is the next
step if the frequency table is to drive the order rather than confirm it.

### Step 1 evidence (recorded 2026-11-22, issue #651)

Item 1 landed. The corpus golden moved from **25 of 50 accepted (50.0%)** at the front end and
**12 of 50 (24.0%)** through the pipeline to **33 of 50 (66.0%)** and **20 of 50 (40.0%)**. Eight
files changed state, and each one is a row of item 1:

| Fixture | Before | After |
|---|---|---|
| `components/arrow-component.tsx` | `PARSE_UNEXPECTED_TOKEN` | accepted, pipeline ok |
| `components/export-default.tsx` | `SUBSET_IMPORT` | accepted, pipeline ok |
| `components/react-default-import.tsx` | `SUBSET_IMPORT` | accepted, pipeline ok |
| `components/react-namespace-import.tsx` | `SUBSET_IMPORT` | accepted, pipeline ok |
| `components/strict-mode.tsx` | accepted, `IR_UNSUPPORTED` | accepted, pipeline ok |
| `expressions/nullish-coalescing.tsx` | `SUBSET_OPERATOR` | accepted, pipeline ok |
| `hooks/use-callback.tsx` | `REACT_UNSUPPORTED_API` | accepted, pipeline ok |
| `hooks/use-id.tsx` | `REACT_UNKNOWN_HOOK` | accepted, pipeline ok |
| `hooks/use-layout-effect.tsx` | `REACT_UNKNOWN_HOOK` | accepted, pipeline ok |

**What the ADR got wrong, and what the landing corrected.**

1. **`??` and `?.` are not "IR expressions with a type rule" — they are two new expression kinds.**
   The ADR assumed they could lower to an existing form. They cannot: the IR has no null literal, so
   there is nothing to compare an `optional<T>` against, and a `conditional` needs a `bool` test.
   `coalesce` and `optionalField` are additive kinds (a module that writes neither is byte-identical,
   so `irVersion` stays 1), and the evaluator and codegen implement the same short-circuit.
2. **`useId` is an `environment` value, not a new hook kind.** The ADR described it as "a stable id
   from the instance key and the call ordinal" without saying where it lives. It fits the existing
   `environment[]` shape with a new `id` provider, which is what let it land without a second
   mechanism: the generated base derives the value in `build()` from `node_id_base_` and the
   declaration order, and it registers no host consumer because nothing pushes it.
3. **`useId` was unreachable without a schema change.** The hook's whole purpose is an
   `id`/`aria-labelledby` pair, and `id` was a literal-only selector target while `aria-labelledby`
   did not exist at all. Both had to change for the hook to be usable, which the ADR did not
   anticipate: a hook can be accepted and still be pointless if the attributes it feeds are missing.
4. **`useMemo`/`useCallback` desugar in the parser, not the rules.** The ADR left the mechanism open.
   Desugaring where the wrapped expression is still a parse tree means every later pass sees the
   ordinary form it already handles, so there is no second path to keep in step; a form outside the
   safe shapes is left alone so the rules report it with the message that names the accepted spelling.
5. **`StrictMode`/`Profiler` are spliced, not built.** "Pass-through" needed a concrete meaning: they
   contribute no node, so their children are spliced where the element stood, exactly as a fragment's
   are. A wrapper as the returned root resolves to its single child.

**Still open from item 1.** `autoFocus` on an input stays `HTML_ATTR` (the prop exists on `View`, not
on the input elements), which the spike already recorded. `use-memo.tsx` still fails on
`TESSERA_JSX_SUBSET_ARRAY_METHOD` because its body uses `.filter`, which is item 3.

### Step 2 evidence (recorded 2026-11-22, issue #652)

Item 2 landed. The corpus golden moved from **33 of 50 accepted (66.0%)** at the front end and
**20 of 50 (40.0%)** through the pipeline to **34 of 50 (68.0%)** and **21 of 50 (42.0%)**:
`handlers/handler-control-flow.tsx` now compiles, which is the one fixture written for this shape.

**What the ADR got wrong, and what the landing corrected.**

1. **The `if` action needed a `guard`, not a new `cond` field.** The ADR described "a pure `cond` and
   two action lists". Every action already carries a pure bool `guard` (issue #236), so the condition
   reuses it and the two lists are the only new fields. That keeps the format smaller and means the
   validator's existing guard check covers the condition too.
2. **`else if` is not a separate form.** The ADR listed `if`/`else` without saying how a chain
   lowers. Nesting the `If` directly in the else branch needs no new kind and no parser special case
   beyond one branch in `parse_branch_block`, so `else if` is free.
3. **A branch is a `Block` even when it is one statement.** The parser wraps a bare statement in a
   block, so the lowering has one shape to walk and a nested `if` is an `If` inside a list rather
   than a special case.
4. **`const` in a handler body is the component-level local, not a new binding.** The ADR said
   "lowered to a conditional action in the handler IR" for control flow but did not say what a
   handler `const` becomes. It is a local inlined at each use (decision 8), so it produces no action
   at all; `ComponentNames::add_const_local` is the one new seam.
5. **An early `return;` is not an action.** It ends the list it appears in, which is what makes the
   corpus fixture's `if (disabled) { return; }` an `If` with an empty `then` list. A `return <value>;`
   stays a preview-body return and is rejected in an inline handler.

**Still open from item 2.** `let`, loops, `switch` and mutation stay in native handlers, as the ADR
says. A loop is a parse error (`TESSERA_JSX_SUBSET_STATEMENT`), because the statement grammar has no
loop form at all.

### Step 3 evidence (recorded 2026-11-22, issue #653)

Item 3 landed. The corpus golden moved from **34 of 50 accepted (68.0%)** at the front end and
**21 of 50 (42.0%)** through the pipeline to **38 of 50 (76.0%)** and **24 of 50 (48.0%)**. Four
files changed state: `expressions/string-methods.tsx`, `expressions/list-length.tsx`,
`expressions/object-spread.tsx`, and one that used `.filter` in a render expression.

**What the ADR got wrong, and what the landing corrected.**

1. **`filter` and `slice` are IR kinds, not allowlist entries.** The ADR said "the pure-expression
   allowlist gains `filter` and `slice` on lists". A list operation is not a call: it binds a name
   (`filter`'s item) and returns a list, which no existing kind expresses. They are two additive
   kinds, `listFilter` and `listSlice`, so `irVersion` stays 1.
2. **The string methods are calls with the receiver as the first argument.** The ADR listed them
   beside the numeric functions without saying how a method becomes a call. Making the receiver
   argument 0 means the evaluator, the validator and codegen see one shape for `s.trim()` and
   `trim(s)`, and the allowlist stays a flat list of names.
3. **`.length` needed a lowering rule, not just an allowlist entry.** `length(list)` already existed;
   the property did not. It is a member access that lowers to the existing call, so no new kind and
   no new function.
4. **A record spread is a `recordUpdate`, not a `record`.** The ADR said "object spread `{...state, x}`
   is accepted for record values with known fields". A `record` value must write every required
   member; an update starts from a complete base and writes only what changes. The validator's
   missing-member rule therefore skips an update, and the IR carries the base's ref id rather than a
   copy of the whole record.
5. **`parseIntOr` and `parseFloatOr` did not land.** The ADR lists them with the string functions.
   They are not total in the same way — each needs a fallback and a parse-failure rule — and no
   corpus file uses either, so they stay open rather than being guessed at.

**Still open from item 3.** `sort` stays rejected (it mutates its receiver). An inline object type in
a prop (`rows: { id: string }[]`) is still `TESSERA_IR_UNSUPPORTED`: that is an anonymous record type,
which the type table does not have, and it is what keeps `filter-map.tsx` and `slice-map.tsx` out.

### Step 4 evidence, part 1 (recorded 2026-11-22, issue #654)

The reducer form parses; the `useReducer` and `useContext` lowerings are still open, so the corpus
does not move (38 of 50 at the front end, 24 through the pipeline, unchanged from step 3).

**What the ADR got wrong, and what the landing corrected.**

1. **A reducer is a module-scope helper, and helpers were skipped, not parsed.** The ADR describes
   `reduce` as "a pure function over a record state and a closed action union" without saying where it
   lives. It is a lowercase `function` at module scope, which the parser classified as a helper and
   skipped wholesale (issue #660's fix for unrelated helpers). The body is now parsed when it declares
   a `switch`, and skipped otherwise, so the two behaviours coexist.
2. **A helper needed more than one parameter.** The component parameter parser accepted exactly one,
   because a component takes one props object. A reducer takes `(state, action)`, so a helper now
   accepts a list.
3. **`switch` is a statement form, not an expression.** It is parsed by the same block-statement
   dispatch as `if`/`const`, and each case is a `Block` whose first operand is its label, so the
   lowering reads the label and the statements from one node.
4. **`default` must be last, and `break` is meaningless.** A closed action union has no case to fall
   to, so a `case` after `default` is unreachable and is rejected; every case of a reducer returns, so
   `break` is accepted and ignored rather than being a form the lowering has to model.

**Still open from item 4.** The static `useContext` provider tree, and the generated C++ for a
reducer state (below).

### Step 4 evidence, part 2 (recorded 2026-11-22, issue #801)

`useReducer` is recognised and lowered. The corpus moves to 39 of 50 at the front end (78.0%) and
26 through the pipeline (52.0%), from 38/24.

**The shape, as it landed.** Accepted only when `reduce` is a module-scope `function` the same file
declares, taking `(state, action)` with declared types; the state type is a record alias; the action
type is a union of object literals each carrying a `type` string literal and nothing else; the
helper returns its state type and its body is exactly one `switch` on `action.type`; every case is
one `return <record>;` labelling a `type` the union declares, once; and every `type` in the union has
a case. Anything else is `TESSERA_REACT_REDUCER_FORM`, reported at the part that broke the form.

**A missing case is the form error, not a fall-through.** The handler set is one handler per `type`,
so an action the union declares with no case would give a `dispatch({ type: "x" })` that compiles
and does nothing. A `default` clause is accepted and recorded but adds no dispatch key, because the
table is keyed by the literal.

**Lowering.** One state cell of the state record type (an ordinary record-valued `useState`), one
handler per case, and a `dispatch` table mapping each `type` to its handler. `dispatch({ type: "x" })`
becomes a `Dispatch` action naming the cell and the literal.

**Three layers the case body needed, none optional.** A case returns `{ count: state.count + 1 }`, a
record whose field is computed:

1. The record lowerer accepted literal field values only. `ir-v1-expr.md` section 4.3 already says
   the field is an Expression in the format so dynamic values can land "in a later slice"; this is
   that slice.
2. The IR validator checked record field values against an *empty* scope, so the reference would have
   been reported unresolved. It now takes the component scope and the handler's locals. A `set` of a
   record-typed state also takes a record value — section 4.3's fourth position — without which a
   case's generated handler could not be written at all.
3. A case reads its own state through the symbol the reducer declared, so `state` resolves to
   `state::state` and `state.count` to `state::state.count`. A same-named local shadows that and
   yields a bare `state.count`.

**A pre-existing bug this exposed.** `tools/codegen/diagnose.cpp` copied components, bindings and
stylesheets into the IR module but not the record type table, so every record-typed position on the
preview and corpus path reported `TESSERA_IR_UNKNOWN_TYPE` while the CLI accepted the same file. It
is what kept `object-spread.tsx` out too: one of the two files the pipeline gained was already
accepted by the CLI.

**Still open.** The generated C++ for a reducer state. The IR is valid, but `codegen`'s text-value
typing has no record table, so `<Text>{state.count}</Text>` reports `TESSERA_CODEGEN_UNSUPPORTED`.
One slice: thread the module's record table through `printable_text_expression` and its eight call
sites, and teach `declared_text_type` to resolve a dotted field through it.


## Follow-up
- Update `jsx-subset-v1.md` sections 6, 7, 9, 10 and 12, `ir-v1.md` section 2.1 and `ir-v1-expr.md`
  section 4 in the implementing PRs (the subset spec is near its size limit: edit in place).
- One issue per step of item 6, labeled for the farm. They are `ready` while this ADR is Proposed on
  purpose (see the status line), not after Acceptance.
- Register the new diagnostics (`TESSERA_REACT_REDUCER_FORM`, `TESSERA_REACT_CONTEXT_DYNAMIC`, and the
  existing codes whose messages change) with fixtures.
- Update `react-core-coverage.md` and `react-compat.md` section 4 as each step lands.
