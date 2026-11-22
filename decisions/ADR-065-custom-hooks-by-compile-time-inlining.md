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

## Evidence required before Accepted

This ADR stays Proposed until measured evidence exists. Spike [#659](https://github.com/thnak/Tessera/issues/659) implements the expansion pass off by default and runs it on at least eight fixtures, comparing each with the hand-expanded component (same IR after id mapping), recording generated size and member names, the related-span diagnostics, and the spec rules that were wrong or missing (including `useService` inside a hook). The implementing issues #645 to #648 wait for Acceptance.

### Spike #659 evidence (recorded 2026-11-21)

Built off by default: `FrontEndOptions::custom_hooks` (default `false`) in
`src/compiler/analyzer/include/tessera/compiler/jsx/front_end_options.hpp`; the parser hook
declaration in `src/compiler/analyzer/src/parser_hook.cpp`; the expansion in
`src/compiler/analyzer/src/hook_expansion{,_clone,_substitute}.cpp` behind
`hook_expansion.hpp`. Tests `hook_expansion` and `hook_expansion_differential`. Normative rules:
[`custom-hooks-v1.md`](../docs/specs/custom-hooks-v1.md). With the switch off the pipeline is
unchanged: `parse_component` still reports `TESSERA_REACT_CUSTOM_HOOK`, and the differential test
asserts that rejection.

**The model holds.** The expansion is a parse-tree-to-parse-tree rewrite before the rules, so a
custom hook has no IR form, no generated class and no run-time cost. All eight fixtures lower to the
same IR as the hand-expanded component once the `<hook>_<n>__` prefix is mapped away. Four of the
eight reach that comparison through the full pipeline; the other four stop at the same pre-existing
engine gap on both sides, which is the same evidence in a weaker form (the expansion put the
component in exactly the state the author's hand-written version is in). The differential test
compares the diagnostic text with the prefix and the `line:col` removed, so a difference in *which*
construct is rejected still fails.

| Fixture | Hook source | Hand source | Hook IR | Hand IR | Members produced |
|---|---|---|---|---|---|
| `usePagination` (object return, defaulted parameter omitted) | 347 B | 207 B | 938 B | 853 B | `state::usePagination_1__page` |
| `useToggle` (tuple return) | 296 B | 185 B | 884 B | 819 B | `state::useToggle_1__open` |
| form field with an effect | 315 B | 211 B | 1060 B | 976 B | `state::useField_1__value` |
| a hook calling a hook | 420 B | 218 B | — | — | `state::useDoubled_1__count` (blocked, see below) |
| two calls of one hook | 317 B | 278 B | 1422 B | 1327 B | `state::useFlag_1__on`, `state::useFlag_2__on` |
| a hook returning a ref for a behavior target | 265 B | 171 B | — | — | `ref::usePressable_1__r` (blocked, see below) |
| a hook with a callback parameter | 335 B | 235 B | — | — | `state::useAction_1__busy` (blocked, see below) |
| a hook with an expression argument in an effect dependency | 308 B | 215 B | — | — | `state::useWatch_1__seen` (blocked, see below) |

The IR column is the canonical `ir::to_text(ir::to_json(component))` with the `source` members
removed, because a hook's statements live in the declaration and the hand-written ones live in the
component, so those byte offsets differ by construction. Every other field is compared.

**Generated size.** The hook form is 1.2× to 1.9× the hand-written source (the declaration is
written once and the call site is short; the hand-written form repeats the body). The IR is 1.07× to
1.10× the hand-written IR, and the whole difference is the longer member names: the expansion adds no
node, no handler and no state entry the hand-written component does not have. Two calls of one hook
produce two states and two handlers in both forms, which is the property the fixed-slot model needs.

**What the engine was missing.** Four gaps, all pre-existing and none caused by the expansion; each
one blocks a fixture on *both* sides, so the spike records it rather than working around it:

1. **A `const` local is never inlined.** `docs/design/jsx-ir-lowering.md` decision 8 says
   "`local::x` is always inlined, so no `local::` ref is emitted", and
   `ComponentNames::resolve` returns nullopt for a local on purpose ("that pass is not part of slice
   6"). No test anywhere lowers a plain `const x = pure_expr`, so the inlining is unimplemented, not
   merely untested. This is what blocks fixtures 4 and 6: `const doubled = c.count * 2` and
   `const press = usePressFeedback(r, ...)` are locals. It also means an expression argument to a
   hook (spec section 4 step 4, which creates a `const <prefix><param>` prologue local) cannot lower
   yet, which is why fixture 8 passes a plain name.
2. **A behavior hook's result is not a resolvable name.** `usePressFeedback` returns a record whose
   fields the analysis does not enter into the symbol table, so `press.pressed` is
   `TESSERA_IR_UNRESOLVED` on both sides. This is the same slice-4 gap, seen through a library hook.
3. **A native handler body must be a preview body.** `TESSERA_REACT_PREVIEW_BODY` rejects
   `const go = (): void => { log(1); }` on both sides, so fixture 7 cannot reach lowering. The
   expansion is not involved: the same statement is rejected when written by hand.
4. **A local cannot be an effect dependency.** `TESSERA_REACT_EFFECT_DEPS` rejects `[limit]` when
   `limit` is a `const` local, on both sides. This is the rule that makes spec section 4's
   "Effect dependencies" paragraph necessary, and it is why the expansion must reject an expression
   argument to a parameter that appears in a dependency list.

**Spec rules that were wrong or missing.** Four, all found by building it:

1. **The rename scheme collides with the hook-name rule.** Spec section 4 step 3 names every
   introduced name `<hook>_<n>__<name>`, so `useToggle`'s setter becomes `useToggle_1__setOpen`. The
   rules' `is_hook_name` was `name.starts_with("use")`, which classifies that as a hook call and
   reports `TESSERA_REACT_UNKNOWN_HOOK` on the *expanded* component. The spec must say that an
   expansion name is never a hook call, and the rules now exclude it explicitly
   (`is_expansion_name` in `src/compiler/rules/src/expr_walk.hpp`). This is a real defect in the
   spec's own naming rule, not an implementation slip.
2. **A parameter used as an effect dependency cannot take an expression argument.** Section 4 says an
   expression argument becomes a `const` prologue local, and section 4's "Effect dependencies"
   paragraph says a dependency must be a prop, state variable or animated value. The two rules
   contradict each other for a parameter that appears in a dependency list. The spike adds
   `TESSERA_REACT_HOOK_DEP` and rejects the call site with a hint to bind the expression in the
   component; the spec needs that rule written down.
3. **A defaulted parameter may be omitted, and section 2 does not say so.** The spike accepts a
   trailing parameter with a literal default and substitutes the default (fixture 1 omits
   `pageSize`). Section 2 declares defaults but section 4 step 4 says "pass every declared
   parameter", which would make a default useless. The spec should say the default is substituted.
4. **`useService` inside a hook is still undecided.** Nothing in `src/compiler` handles `useService`
   at all (only `emit_services.cpp`/`emit_header.cpp` in codegen consume `ir::Service`), so the
   expansion has nothing to deduplicate and the spike leaves the open item open. The decision the
   spec needs is whether two hooks that each call `useService(IFoo)` produce one service entry or
   two; the fixed-slot model argues for one per interface, keyed by the interface type, because a
   service is a singleton by definition and a per-call entry would allocate twice for one service.

**Related spans.** The expansion records a `CallSiteMap` of `{spliced span, call span, hook}` per
call site, and `Expander::related_for` turns a span inside a hook into
`RelatedSpan{call site, "expanded here, from <hook>_<n>"}`. The expansion's own diagnostics are
reported at the call site already, so they carry no related span; the map exists for the rules, which
are not yet wired to it. That wiring is the one piece of spec section 8 the spike does not finish:
the map is built and tested (`test_the_call_site_map_names_the_expansion`), the rules do not consume
it yet.

**What this does not settle.** Import-following for hooks (a hook declared in another file) is not
implemented: `collect_hook_declarations` reads the module it is given. The size limits (depth 8, 400
statements) are enforced and tested, but the 200-statement warning is not emitted as a warning
because the diagnostic sink has no warning path for this stage. `useService` is undecided (above).
The four engine gaps are slice-4 work, not expansion work, and the spike does not touch them.

### Addendum (2026-11-22): the implementing issues landed

#645 to #648 landed in PR #759, so this ADR's evidence is now the shipped behaviour rather than a
spike. What changed since the record above:

- **The switch is on by default.** `FrontEndOptions::custom_hooks` defaults to `true`; the
  `TESSERA_REACT_CUSTOM_HOOK` rejection is the switch-off path only, and it keeps a fixture
  (`Entry::kNoCustomHooks`) so the code stays covered.
- **Engine gap 2 is closed.** A behavior hook's result now resolves: `beh::` is a reference form in
  the validator, in codegen and in `declared_text_type`, so `press.pressed` lowers instead of
  reporting `TESSERA_IR_UNRESOLVED`. Gaps 1, 3 and 4 remain slice-4 work.
- **The rules consume the call-site map.** `analyze_module` takes the `CallSiteMap` and attaches the
  related span, which is the piece of spec section 8 the spike left unfinished.
- **`useService` inside a hook is recorded as unreachable, not undecided.** `useService` has no
  front-end form at all (no manifest entry, no core-hook entry, no analyzer path), so
  `TESSERA_REACT_HOOK_SERVICE` guards a form that cannot be written. The spec says so; the code is
  reserved. The dedupe question above is therefore moot until `useService` has a front-end form.
- **The spec's four corrections are written down**, including the rename/`is_hook_name` collision
  and the defaulted-parameter rule.

This does not move the ADR to Accepted. Only the owner does that.

## Follow-up

- `docs/specs/custom-hooks-v1.md` accepted; update the sentences that say custom hooks are rejected
  (`jsx-subset-v1.md` section 7, `jsx-subset-v1-jsx.md`, `compiler.md` section 4, `ui-model.md`,
  `component-library.md`, `developer-workflow.md`, `project-generator.md`) in the implementing change.
- The expansion pass (parse tree to parse tree), import-following for hooks, diagnostics with related
  spans, naming of generated members, and conformance and differential tests.
- ADR-058: record the answer to question 3 for custom hooks.
