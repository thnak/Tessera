# ADR-066: React core additions for V1

- Status: Proposed (2026-11-20). Owner direction: "agreed" to the V1 proposals of
  [`react-core-coverage.md`](../docs/reference/react-core-coverage.md). That table is the detailed
  record; this ADR is the decision that fixes the set, the order and the IR changes it needs. The L
  rows are in [ADR-067](ADR-067-pending-ui-transitions-suspense-lazy.md), custom hooks in
  [ADR-065](ADR-065-custom-hooks-by-compile-time-inlining.md).
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
6. **Order.** (1) cheap acceptances; (2) custom hooks (ADR-065); (3) handler control flow; (4) list and
   string expressions and record spread; (5) `useReducer` and `useContext`; the L rows of ADR-067 run in
   parallel with (3) to (5) because they touch different stages. Each step lands as one PR that closes
   its issues, with its spec text, tests and diagnostics (`AGENTS.md`, landing size).
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

## Follow-up

- Update `jsx-subset-v1.md` sections 6, 7, 9, 10 and 12, `ir-v1.md` section 2.1 and `ir-v1-expr.md`
  section 4 in the implementing PRs (the subset spec is near its size limit: edit in place).
- One issue per step of item 6, labeled for the farm after this ADR is Accepted.
- Register the new diagnostics (`TESSERA_REACT_REDUCER_FORM`, `TESSERA_REACT_CONTEXT_DYNAMIC`, and the
  existing codes whose messages change) with fixtures.
- Update `react-core-coverage.md` and `react-compat.md` section 4 as each step lands.
