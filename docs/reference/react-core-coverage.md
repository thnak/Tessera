# React core coverage: what is kept, what is proposed for V1, what stays out

Status: inventory with proposals, 2026-11-20. It consolidates scattered statements
([`react-compat.md`](../specs/jsx-intrinsics-v1/react-compat.md) section 4,
[ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md),
[ADR-058](../../decisions/ADR-058-react-vite-port-compatibility-tiers.md),
[ADR-065](../../decisions/ADR-065-custom-hooks-by-compile-time-inlining.md),
[`jsx-subset-v1.md`](../specs/jsx-subset-v1.md)) into one table per area, and adds a V1 proposal and a
cost for each row. The owner's direction is that the core features of React must be kept. A proposal
here is not a decision until the owner accepts it and the owning spec is changed.

Legend. **Today**: what the front end does now, checked in `src/compiler` where stated, otherwise as the
spec says (the spec can be ahead of the code). **V1**: `keep` = already accepted, `add` = proposed for
V1, `later` = proposed after V1 with a stated blocker, `no` = stays rejected, with the Tessera form to
write instead. **Cost**: S = a parser or rule change; M = a new lowering or IR addition; L = a design
that changes the model.

**Measured, not assumed.** Spike #660 ran the front end and the whole single-file pipeline over a
50-file corpus of React spellings (`tests/fixtures/corpus/react/`, golden
`tests/golden/corpus/react-front-end.txt`) and over this repository's own React sources. The
frequency tables and the corrections they force are in
[ADR-066](../../decisions/ADR-066-react-core-additions-for-v1.md#spike-660-evidence-recorded-2026-11-21).
Three corrections apply to the tables below and are marked **measured** where they occur: several
rows marked `rejected` are accepted by the *front end* and rejected later by the schema or the
lowering, so the work belongs to a different stage; `memo` already compiles; and the largest single
rejection reason is a parser gap (`TESSERA_JSX_PARSE_UNEXPECTED_TOKEN`) that this table does not
list at all.


A second corpus (43 files in everyday TypeScript idiom, [measurement note](../design/react-port-corpus-measurement.md))
adds the rows marked **measured, corpus 2** below: type-grammar and function-value forms the first
corpus did not exercise. As written it is accepted 34.9% at the front end and 4.7% through the pipeline.

## 1. Hooks

| React feature | Today | V1 | Cost | Notes |
|---|---|---|---|---|
| `useState` | accepted (checked) | keep | | Typed: `useState<int>` |
| `useEffect` | accepted (checked); exact dependency list, optional cleanup | keep | | Body is a native handler |
| `useRef` | accepted (checked); no `.current`; `focus()` and `blur()` as handler statements | keep | | Callback refs and `useImperativeHandle` below |
| Custom hooks | accepted: expanded at each call site (ADR-065) | done | M | Inlined at each call site, no IR change |
| `useMemo`, `useCallback` | rejected (`UNSUPPORTED_API`, checked); spec decided (Q16) to lower safe forms | add | S | Pure-expression memo becomes a local; setter-only callback becomes an inline handler |
| `useReducer` | rejected (checked) | add, limited | M | A reducer as a pure function over a record with one `switch` on a literal action type; lowers to one handler per action type (ADR-058 rule 4). Needs handler control flow (section 5) and records (ADR-051) |
| `useContext`, `createContext` | rejected | add, static | M | Lower to an IR `Environment` with a typed default and a compile-time provider tree (ADR-058 rule 4). The provider value must be a pure expression of its component's props and state |
| `useId` | rejected | add | S | A stable id from the instance key and the call ordinal (the automation id rules of `ir-v1.md` section 3.1.1) |
| `useLayoutEffect` | rejected | add as `useEffect` | S | There is no browser paint to order against; document that it has `useEffect` timing |
| `useSyncExternalStore` | rejected | no (use `useObservable`, `useService`) | | `docs/design/observable.md`, ADR-050; external state belongs to services |
| `useTransition`, `useDeferredValue` | rejected | later | L | Needs the async and scheduling model (`async-data-v1.md`); an alias returning "never pending" would hide that |
| React 19 `use`, `useActionState`, `useFormStatus`, `useOptimistic` | rejected | no | | Forms have their own spec (`forms-and-validation-v1.md`) |
| `useImperativeHandle`, `forwardRef` | rejected | no | | Function values; a component exposes a callback prop instead |
| `useInsertionEffect`, `useDebugValue` | rejected | no | | No use in this model |
| `useAnimatedValue`, `useService`, `useObservable`, `useBreakpoint`, `useTheme` | Tessera-only | keep | | Not React; listed so the two sets are not confused |

## 2. Components and JSX

| React feature | Today | V1 | Cost | Notes |
|---|---|---|---|---|
| Function component, props, destructured props, prop defaults | accepted | keep | | |
| Arrow-function component, `export default function` | rejected (subset 6, 12) | add | S | The most common spellings in ported code; same rules as a function declaration |
| `import React from "react"`, `import * as React` | rejected (Q15 accepts three named hooks only) | add as ignorable | S | The automatic JSX runtime makes the import unused; any other name from `react` stays an error |
| `children` of a user component | decided (O2, static `Children` slot); `Slot` appears in the IR and codegen, front-end acceptance not re-checked | keep | | `ir-v1.md` section 3.4 |
| `key` on list items | accepted | keep | | `key` to force a remount: no, use `resetKey` of `ErrorBoundary` |
| Fragment as a child | accepted | keep | | |
| Fragment as the returned root, keyed Fragment | rejected (Q18) | no | | Wrap in `View`; an implicit box changes layout |
| `memo(Component)` | accepted (measured: it is a call whose result is a `const`) | keep | S | Updates already follow dependencies; `memo` is accepted so ports compile, with no effect |
| `StrictMode`, `Profiler` | rejected (`TESSERA_IR_UNSUPPORTED`, measured: the front end accepts the tag) | add as pass-through | S | No development double-render in this model |
| Spread props `{...props}` | rejected | no | | Keys must be known at compile time; pass named props |
| Render props, function as a child | rejected (`TESSERA_IR_UNSUPPORTED`, measured: the front end accepts the call) | no | | Function values; use `children` slots or named element props (later, ADR-064) |
| `lazy`, `Suspense` | rejected | later | L | Needs code splitting and an async data model; there are no JS bundles |
| Portals | rejected | no | | The Layer host (ADR-064 tier 0) and `Overlay` cover it |
| Class components, `async` components | rejected | no | | By design (ADR-036) |
| Error boundaries | `ErrorBoundary` intrinsic with `fallback` | keep | | Not the class lifecycle API |
| Qualified type names `React.ReactNode`, `React.FC<P>`, `React.MouseEvent` | rejected (`TESSERA_JSX_PARSE_UNEXPECTED_TOKEN`, measured, corpus 2: 3 of 43 files) | add | S | Not a React feature: the type grammar has no `A.B`. Resolve to the named type or accept and ignore `React.`; `children` typed `ReactNode` is `TESSERA_IR_SLOT` (write `Children`) |
| `as const` on a returned tuple, `forwardRef<T, P>(...)` generic call | rejected (`TESSERA_JSX_PARSE_MISSING_SEMICOLON`, measured, corpus 2: 3 files) | `as const` add; `forwardRef` stays no | S | `as const` appears in custom hooks that return a tuple |
| Rest in a destructuring pattern, array spread `[...rows]` | rejected (`TESSERA_JSX_PARSE_UNEXPECTED_TOKEN`, measured, corpus 2: 2 files) | no | | Keys must be known at compile time (spread props); `[...rows].sort()` copies to sort, and `sort` is later |
| Bare `number` type | rejected (`TESSERA_JSX_TYPE_NUMBER`, measured, corpus 2: the first error of 11 of 43 files) | no (decided) | | Write `int` or `float` (`jsx-subset-v1.md` section 5). Listed because it hides every later row in a port: the measurement note reports a second run with it rewritten |
| Functional state updater `setN(prev => prev + 1)`, inner `function`, `memo(function Row() {})` | rejected (`TESSERA_IR_UNSUPPORTED` or `TESSERA_JSX_SUBSET_FUNCTION_VALUE`, measured, corpus 2: 3 of 43 files) | add the updater (M); named function expression as an argument: later | M | The updater is the most common React state idiom: it reads the cell and writes the cell, so it lowers to `set(cell, f(cell))` for a pure arrow |
| `createContext("x")` without a type argument, `useState(prop)` without one | rejected (`TESSERA_REACT_CONTEXT_FORM`, `TESSERA_IR_UNSUPPORTED`, measured, corpus 2: 2 files) | add inference from a literal or a typed prop | S | The message names the fix; the cost is a port edit, not a missing form |
| Component-body `if` early return, `switch`, `for` | rejected (`TESSERA_JSX_SUBSET_STATEMENT`, measured, corpus 2: 3 of 43 files) | no (decided; handler control flow is section 5) | | The most common reason a real component is not a single return expression |
| Module-scope helper functions | accepted (a lowercase `function` is a helper, not a component) | keep | | Not a React form: the parser used to treat every `function` declaration as a component, so a lowercase helper was a badly named component. A lowercase name now makes it a helper, whose body is skipped rather than lowered |
| Destructured local `const { a } = b` | accepted (a rename inside the pattern is refused) | keep | | Not a React form: the parser accepted only a name or `[` after `const`. Each listed field binds the field of the same name; `{ a: c }` is `TESSERA_JSX_SUBSET_STATEMENT` because the subset has no rename form |
| `readonly` type modifier | accepted and dropped | keep | | Not a React form: the type grammar had no `readonly`. It changes nothing the IR can express, so it is accepted rather than rejected |
| JSDoc comment inside a props type | accepted | keep | | The lexer already skips comments as trivia, so a documented prop parses; the corpus measurement's claim that it did not was wrong |


## 3. Events, forms, refs

| React feature | Today | V1 | Cost | Notes |
|---|---|---|---|---|
| `onClick`, `onChange`, `onFocus`, `onBlur`, `onKeyDown`, `onKeyUp`, `onSubmit` (inputs) | accepted | keep | | `onChange` payload is a record (Q1, option C) |
| `onMouse*` | rejected (`TESSERA_IR_UNKNOWN_PROP`, measured: the front end accepts the attribute) | no | | Fires for mouse only; use the pointer events. A schema row, not a parser row |
| `onDoubleClick`, `onContextMenu`, `onWheel`, `onDrag*`, `onTouch*` | rejected (`TESSERA_IR_UNKNOWN_PROP`, measured: same) | later | M | Through the behavior hooks (long press, pan, context menu) of ADR-064, not as raw aliases |
| `e.preventDefault()`, `e.stopPropagation()` | native handlers only (Q3); a native body is `TESSERA_IR_UNSUPPORTED` (measured) | keep | | |
| Controlled inputs, `value`, `defaultValue`, `placeholder`, `disabled`, `readOnly` | accepted | keep | | `value` without `onChange` is a warning |
| `checked`, `type="checkbox" / "radio" / "range"`, `select` | rejected (`TESSERA_IR_UNKNOWN_PROP`, measured: the front end accepts the attributes) | later | M | Provided by the library components Checkbox, RadioList, Switch, Selector (ADR-064 tiers 3 to 5), not as intrinsics. A schema row |
| `label htmlFor` | rejected (`TESSERA_IR_UNKNOWN_PROP`, measured: the front end accepts the attribute) | later | M | Becomes the association of `FieldLabel` and its control in the Field stack. A schema row |
| Callback refs | rejected | no | | Function values |
| `autoFocus` | rejected (`TESSERA_IR_UNKNOWN_PROP` on `<TextInput>`, measured; `<View>` already declares it) | add | S | A focus request at mount; the behavior is defined with `useFocusTrap` and focus management. A schema row: the prop exists on `View` and not on the input elements |

## 4. Rendering expressions

| React feature | Today | V1 | Cost | Notes |
|---|---|---|---|---|
| `cond && <X />`, `cond ? <A /> : <B />` | accepted; `cond` must be `bool` | keep | | No truthiness; the diagnostic names the fix |
| `.map(item => <X key=... />)`, single expression body | accepted | keep | | `VirtualList` for large lists |
| `.filter(...).map(...)`, `.slice()`, `.sort()` in JSX | rejected (`TESSERA_JSX_SUBSET_ARRAY_METHOD`, measured: the first rejection of 3 of 50 corpus files) | add `filter` and `slice`, and `.length` as `length(list)`; `sort` later | M | A pure list expression with a pure arrow predicate in the IR allowlist. Real, but not the largest gap: the parser gaps below outrank it |
| Block-body `.map`, `if` in render | rejected | no | | Render stays an expression; compute into `const` locals |
| Template strings | accepted | keep | | |
| `??`, `?.` | rejected (`TESSERA_JSX_SUBSET_OPERATOR`, measured) | add | S | They map onto `optional<T>`: `a ?? b` and `a?.b` as IR expressions with a type rule |
| String and number methods (`trim`, `toFixed`, `parseInt`) | rejected (`TESSERA_IR_UNSUPPORTED`, measured: the front end accepts the call) | add a small allowlist | M | Total functions only (`trim`, `toUpperCase`, `toLowerCase`, `startsWith`, `slice`, `toFixed`, `parseIntOr`, `parseFloatOr`). A lowering row, not a parser row |
| Object spread `{...state, x}` | rejected (`TESSERA_JSX_SUBSET_LITERAL`, measured) | add for records | M | Record update with known fields; needed by `useReducer` |
| `.includes`, `.reduce`, `Math.max`/`Math.min`, `src={url}` | rejected (`TESSERA_JSX_SUBSET_ARRAY_METHOD`, `TESSERA_IR_UNSUPPORTED`, measured, corpus 2: 4 files) | later | M | Total functions of the string and number allowlist (section 4); `reduce` is a loop with an accumulator and stays native |
| `style={{...}}` literal | accepted | keep | | A computed or spread style: no (keys must be static) |
| `dangerouslySetInnerHTML`, `title`, `data-*` | rejected | no | | No V1 meaning |

## 5. Language inside handlers and components

| Feature | Today | V1 | Cost | Notes |
|---|---|---|---|---|
| Setter-call arrow handlers | accepted | keep | | |
| `if`, `const`, early `return` in handlers | rejected (subset 9.2) | add | M | A conditional action in the handler IR; ADR-058 rule 4 proposes it. Prerequisite for `useReducer` |
| `let`, `for`, `while`, `switch`, `try` in handlers | rejected | `switch` only with `useReducer`; others no | M | Loops and mutation stay in native handlers |
| `async`, `await`, `fetch` in components | rejected | no | | Data comes through services and observables (ADR-050); async handlers are native |
| Native handler escape hatch | `// Note: bind::<id>` | keep | | The designed route for logic outside the subset |
| TypeScript: `interface`, `enum`, generics, utility types | partial (`type` aliases, unions, records) | per ADR-058 typing rules | M | Tracked there |

## 6. Order of work

The rows marked `add` have dependencies. This is the order that opens the most React code per unit of
work, **as measured by spike #660** rather than assumed: the frequency tables are in
[ADR-066](../../decisions/ADR-066-react-core-additions-for-v1.md#spike-660-evidence-recorded-2026-11-21).
The measurement changed the order: the parser gaps that this table did not list are now step 1,
because they are the first rejection of more files than any single React row.

1. **Parser and type-grammar gaps (S), measured first.** `TESSERA_JSX_PARSE_UNEXPECTED_TOKEN` is the
   largest single rejection reason (11 occurrences, the first rejection of 6 of 50 corpus files and
   of 34 of 58 rejected files in this repository's own React sources). Its causes are not React
   forms: a destructured local, a `readonly` modifier, a JSDoc comment inside a props type, and a
   module-scope helper function. The helper is the largest of the four: every `function` declaration
   is parsed as a component, so 13 of this repository's React files are rejected as badly named
   components. Fixing these unblocks more files than any item-1 row and is a prerequisite for
   measuring anything else in real code.
2. **Cheap acceptances (S):** arrow components and `export default`, `import React`, `StrictMode`,
   `useId`, `useLayoutEffect`, `??` and `?.`, `useMemo` and `useCallback` safe forms. `memo` is
   already accepted and drops off this list. `autoFocus` moves to the schema: the prop exists on
   `View` and not on the input elements, so the port's error is `TESSERA_IR_UNKNOWN_PROP`, not a
   parser rejection.
3. **Custom hooks (ADR-065, M).**
4. **Handler control flow (M):** `if`, `const` and early return as IR actions. It unblocks `useReducer`
   and removes the most common reason a port needs a native handler.
5. **List and string expressions (M):** `filter`, `slice`, `length`, the string allowlist, object spread.
   Real (the first rejection of 3 of 50 files) but not dominant, so it follows the parser gaps.
6. **`useReducer` limited, `useContext` static (M).**
7. **Later (L):** `useTransition`, `Suspense` and `lazy`, raw gesture events.

Items 4 and 5 change the IR allowlist and the handler IR, so each needs a short ADR or a spec change
before code; items 1, 2, 3 and 6 do not change the IR except `Environment` for context.

**Second corpus (43 files, 2026-11-27).** The data adds a step 1b before the rest: the next layer of
type-grammar gaps (qualified names, `as const`, rest and array spread) and function-value forms, which
no row above named; the rows are marked **measured, corpus 2**. Reducer and context are the smallest
groups in the data and stay last. See [the measurement note](../design/react-port-corpus-measurement.md).

**What the measurement also showed.** 12 of the 21 files the front end accepts fail later, and the
reasons are schema and lowering rows (`TESSERA_IR_UNKNOWN_PROP`, `TESSERA_IR_UNSUPPORTED`,
`TESSERA_IR_UNRESOLVED`), not React forms. The pipeline accepts 9 of 50 files (18.0%) against the
front end's 21 (42.0%), so a port's first error for `onMouse*`, `checked`/`type`, `label htmlFor`,
`autoFocus`, string methods and render props is not the one this table's `Today` column names. Those
rows are marked **measured** above and belong to the schema and lowering work, not to this ADR's
items.


## 7. What this inventory does not cover

- TypeScript typing rules (ADR-058 section on the typing budget).
- Build tools (Vite, Tailwind, Sass, path aliases) and ecosystem libraries (router, state managers,
  UI kits); ADR-058 classifies them.
- Browser APIs (`window`, `document`, `localStorage`): there is none, and this is by design.
- Whether each `keep` row is exercised by a test: the table states what the specs say, and where checked,
  what the rule code does. A coverage matrix against `tests/` is a separate piece of work.
