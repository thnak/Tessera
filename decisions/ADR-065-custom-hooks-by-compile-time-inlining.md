# ADR-065: Custom hooks are accepted in V1 by compile-time inlining

- Status: Proposed (2026-11-20). The direction is the owner's: "bring custom hooks into V1; the core
  features of React must be kept". The rules below are the proposal to review. Amends
  [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) item 3 (hooks written by app developers
  are rejected) and answers question 3 of [ADR-058](ADR-058-react-vite-port-compatibility-tiers.md)
  for custom hooks only; `useContext` and `useReducer` stay with ADR-058.
- Date: 2026-11-20
- Related: [ADR-040](ADR-040-generated-base-and-developer-class.md) (generated base),
  [ADR-064](ADR-064-astryx-style-component-system.md) (behavior hooks, library component system),
  [`custom-hooks-v1.md`](../docs/specs/custom-hooks-v1.md) (the specification),
  [`jsx-subset-v1.md`](../docs/specs/jsx-subset-v1.md) section 7.
- Terms: a **custom hook** is a function declared in a project or package whose name is `use` plus an
  uppercase letter and whose body is hook calls, declarations and one `return`. **Inlining** is the
  compile-time replacement of a call by the statements of the hook, spliced into the calling component
  at the call position. A **call site** is one call of a custom hook inside a component.

## Context

A custom hook is how React code shares stateful logic: pagination, a form field, a debounced value, a
toggle. It is one of the most used parts of React and the first thing a ported component reaches for.
The V1 front end rejects it (`TESSERA_REACT_CUSTOM_HOOK`), so every such component copies the logic by
hand, and a port of a React + Vite project (ADR-058) fails on the first `useSomething` it finds.

The rejection was never about the idea. A hook has no identity at run time in this model: components
compile to generated C++ with fixed state slots, and hook order is a property of the source, checked
statically. Expanding a hook at its call site keeps both properties, because the result is exactly what
the author could have written by hand. ADR-058 already proposes this approach; this ADR makes the
decision and its limits explicit, and promotes it to V1.

## Decision

1. **Custom hooks are accepted and expanded inline at each call site.** The expansion is a
   source-to-source pass on the parsed module before the rules, lowering and code generation, so those
   stages see an ordinary component and need no change. A custom hook has no IR form, no generated
   class and no run-time cost.
2. **A hook is a function, not a component.** It takes typed parameters, may call core hooks, library
   hooks (including the behavior hooks of ADR-064) and other custom hooks, declares handlers and local
   constants, and returns one value, a tuple of values or an object literal of values. The call site
   binds the result as a whole, as a tuple or as an object pattern.
3. **The rules that make this safe are the rules hooks already have.** A call is unconditional and at the
   top level of a component body or of another hook; order is source order after expansion;
   `TESSERA_REACT_HOOK_ORDER` applies to the expanded component. Recursion and cycles are rejected.
4. **State is per call site.** Two calls of one hook in a component create two sets of state, effects
   and refs, exactly as React does. Generated ids carry the call-site name so they cannot collide.
5. **Returned values are compile-time substitutions, not run-time objects.** `const p = usePagination(...)`
   binds `p` to the hook's returned fields; `p.page` is replaced by the field's source, so no record type
   and no allocation appear. A ref returned by a hook may be used as the `ref` of an element in the
   component, which is how a hook that owns a behavior target (ADR-064) works.
6. **Where hooks live.** In the same file as the component, in another project file imported with a
   relative path, or in a package (for example the first-party library). The import resolution of
   ADR-014 and `lower_project` is reused; exports follow the existing rules for components.
7. **Diagnostics point at both places.** An error inside an expanded hook names the hook's own span as
   the primary location and the call site as the related span, so the author sees where the logic is
   wrong and where it was used.
8. **What stays rejected.** A hook called conditionally or from a handler; recursion; a hook that
   returns a function value other than a declared handler; a hook that closes over component locals
   (it is a function of its parameters); async hooks; `useService` inside a hook (open item of the
   spec). `useContext`, `useReducer`, `memo` and `forwardRef` are not
   decided here.

## Consequences

### Positive

- The most common React reuse mechanism works, so ported components and first-party library code can
  share logic instead of copying it.
- No new IR, no new generated shape, no new run-time cost: the expansion happens once in the front end.
- The library can ship stateful helpers (pagination, form fields, toggles) as plain TSX.
- Hook rules stay statically checkable: order, unconditional call and typed signatures.

### Negative / risks

- Each call site duplicates the hook's code and state in the generated output; a large hook used many
  times grows the generated component. A size diagnostic at a configurable threshold limits surprises.
- Generated member names now include the call-site name; renaming a binding or reordering calls
  changes ids, which affects state carry in the preview and the names of native handler members
  (`bind::` ids inside a hook are prefixed per call site).
- The pass is new front-end code with its own corner cases (parameter substitution, renaming, spans);
  it needs its own conformance tests and differential tests against hand-expanded components.
- Developers used to React may expect behavior this model does not give (a hook returning a closure,
  conditional hooks); the diagnostics must say why and what to write instead.

## Alternatives considered

- **Keep rejecting custom hooks.** Rejected: it blocks the main React reuse pattern and ports.
- **A run-time hook object per call.** Rejected: it needs dynamic state allocation and breaks the fixed
  slot model that keeps updates allocation-free.
- **Compile each hook to its own generated class that a component composes.** Rejected for V1: it needs
  an opaque handle type in the IR and a lifecycle for the hook instance; inlining gets the same
  behavior with neither.
- **Native hooks declared by packages.** Rejected for V1: see ADR-064 and the reasons in
  [`behavior-hooks.md`](../docs/design/behavior-hooks.md) (no Studio support, ABI, supply chain).

## Follow-up

- `docs/specs/custom-hooks-v1.md` accepted; update the sentences that say custom hooks are rejected
  (`jsx-subset-v1.md` section 7, `jsx-subset-v1-jsx.md`, `compiler.md` section 4, `ui-model.md`,
  `component-library.md`, `developer-workflow.md`, `project-generator.md`) in the implementing change.
- The expansion pass (parse tree to parse tree), import-following for hooks, diagnostics with related
  spans, naming of generated members, and conformance and differential tests.
- ADR-058: record the answer to question 3 for custom hooks.
