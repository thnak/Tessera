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

## 1. Hooks

| React feature | Today | V1 | Cost | Notes |
|---|---|---|---|---|
| `useState` | accepted (checked) | keep | | Typed: `useState<int>` |
| `useEffect` | accepted (checked); exact dependency list, optional cleanup | keep | | Body is a native handler |
| `useRef` | accepted (checked); no `.current`; `focus()` and `blur()` as handler statements | keep | | Callback refs and `useImperativeHandle` below |
| Custom hooks | rejected (`TESSERA_REACT_CUSTOM_HOOK`) | add (ADR-065) | M | Inlined at each call site, no IR change |
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
| `memo(Component)` | rejected | add as identity | S | Updates already follow dependencies; `memo` is accepted so ports compile, with no effect |
| `StrictMode`, `Profiler` | rejected | add as pass-through | S | No development double-render in this model |
| Spread props `{...props}` | rejected | no | | Keys must be known at compile time; pass named props |
| Render props, function as a child | rejected | no | | Function values; use `children` slots or named element props (later, ADR-064) |
| `lazy`, `Suspense` | rejected | later | L | Needs code splitting and an async data model; there are no JS bundles |
| Portals | rejected | no | | The Layer host (ADR-064 tier 0) and `Overlay` cover it |
| Class components, `async` components | rejected | no | | By design (ADR-036) |
| Error boundaries | `ErrorBoundary` intrinsic with `fallback` | keep | | Not the class lifecycle API |

## 3. Events, forms, refs

| React feature | Today | V1 | Cost | Notes |
|---|---|---|---|---|
| `onClick`, `onChange`, `onFocus`, `onBlur`, `onKeyDown`, `onKeyUp`, `onSubmit` (inputs) | accepted | keep | | `onChange` payload is a record (Q1, option C) |
| `onMouse*` | rejected (Q21) | no | | Fires for mouse only; use the pointer events |
| `onDoubleClick`, `onContextMenu`, `onWheel`, `onDrag*`, `onTouch*` | rejected | later | M | Through the behavior hooks (long press, pan, context menu) of ADR-064, not as raw aliases |
| `e.preventDefault()`, `e.stopPropagation()` | native handlers only (Q3) | keep | | |
| Controlled inputs, `value`, `defaultValue`, `placeholder`, `disabled`, `readOnly` | accepted | keep | | `value` without `onChange` is a warning |
| `checked`, `type="checkbox" / "radio" / "range"`, `select` | rejected | later | M | Provided by the library components Checkbox, RadioList, Switch, Selector (ADR-064 tiers 3 to 5), not as intrinsics |
| `label htmlFor` | rejected (Q11) | later | M | Becomes the association of `FieldLabel` and its control in the Field stack |
| Callback refs | rejected | no | | Function values |
| `autoFocus` | rejected on inputs | add | S | A focus request at mount; the behavior is defined with `useFocusTrap` and focus management |

## 4. Rendering expressions

| React feature | Today | V1 | Cost | Notes |
|---|---|---|---|---|
| `cond && <X />`, `cond ? <A /> : <B />` | accepted; `cond` must be `bool` | keep | | No truthiness; the diagnostic names the fix |
| `.map(item => <X key=... />)`, single expression body | accepted | keep | | `VirtualList` for large lists |
| `.filter(...).map(...)`, `.slice()`, `.sort()` in JSX | rejected (subset 10.5); `length(list)` exists as a function | add `filter` and `slice`, and `.length` as `length(list)`; `sort` later | M | A pure list expression with a pure arrow predicate in the IR allowlist; the largest gap by frequency in real code |
| Block-body `.map`, `if` in render | rejected | no | | Render stays an expression; compute into `const` locals |
| Template strings | accepted | keep | | |
| `??`, `?.` | rejected | add | S | They map onto `optional<T>`: `a ?? b` and `a?.b` as IR expressions with a type rule |
| String and number methods (`trim`, `toFixed`, `parseInt`) | rejected; `parseIntOr`, `parseFloatOr` decided (Q26) | add a small allowlist | M | Total functions only (`trim`, `toUpperCase`, `toLowerCase`, `startsWith`, `slice`, `toFixed`, `parseIntOr`, `parseFloatOr`) |
| Object spread `{...state, x}` | rejected | add for records | M | Record update with known fields; needed by `useReducer` |
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
work:

1. **Cheap acceptances (S):** arrow components and `export default`, `import React`, `memo`, `StrictMode`,
   `useId`, `useLayoutEffect`, `autoFocus`, `??` and `?.`, `useMemo` and `useCallback` safe forms.
2. **Custom hooks (ADR-065, M).**
3. **Handler control flow (M):** `if`, `const` and early return as IR actions. It unblocks `useReducer`
   and removes the most common reason a port needs a native handler.
4. **List and string expressions (M):** `filter`, `slice`, `length`, the string allowlist, object spread.
5. **`useReducer` limited, `useContext` static (M).**
6. **Later (L):** `useTransition`, `Suspense` and `lazy`, raw gesture events.

Items 3 and 4 change the IR allowlist and the handler IR, so each needs a short ADR or a spec change
before code; items 1, 2 and 5 do not change the IR except `Environment` for context.

## 7. What this inventory does not cover

- TypeScript typing rules (ADR-058 section on the typing budget).
- Build tools (Vite, Tailwind, Sass, path aliases) and ecosystem libraries (router, state managers,
  UI kits); ADR-058 classifies them.
- Browser APIs (`window`, `document`, `localStorage`): there is none, and this is by design.
- Whether each `keep` row is exercised by a test: the table states what the specs say, and where checked,
  what the rule code does. A coverage matrix against `tests/` is a separate piece of work.
