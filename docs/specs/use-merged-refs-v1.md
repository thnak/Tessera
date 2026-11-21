# `useMergedRefs` v1

Status: draft, normative once accepted. Specifies the `useMergedRefs` hook of
[ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7 (issue #630).

This hook is **not** a behavior hook: it is a compiler-level ref-combining feature. It has no engine
behavior, no runtime state and no outputs, because the thing it produces is an assignment the
generated base already writes.

Terms: a **ref** is a `useRef` binding; an **owner** is a ref that receives a node pointer; a
**merged ref** is a ref whose assignment also fills the refs it names.

## 1. Call form

```tsx
const inner = useRef<View>(null);
const forwarded = useRef<View>(null);
const merged = useMergedRefs(inner, forwarded);
return <View ref={merged}>{...}</View>;
```

One `const` binding at the top level of the component body, under the placement and order rules of
every hook. The first operand is the ref the merged ref stands for; every later operand is an owner
it also fills. At least two operands are required, because a merged ref of one is just that ref.

## 2. Why this exists

A component that both forwards a ref to its parent and keeps an internal one has **two owners for one
node**, and a node has **one `ref` attribute**. Without this hook the component must either drop one
owner or write an effect that copies the pointer after the fact, which runs a frame late and misses
the first layout.

The hook is the answer: one `ref` attribute, one assignment, every owner filled.

## 3. Rules

Each rule is normative and has a test in `tests/unit/lowering_hooks_tests.cpp` (the lowering) or
`tests/unit/codegen_behaviors_tests.cpp` (the generated code).

1. **The merged ref is the primary ref.** The lowering records the aliases on the ref named first; it
   does not create a second ref entry. A second entry would give the node two `ref` attributes and the
   second assignment would overwrite the first.
2. **Every operand must be a ref.** An operand that is not a `useRef` binding is
   `TESSERA_REACT_HOOK_FORM`, because a forwarded ref that never fills is a bug the developer cannot
   see.
3. **At least two operands.** `useMergedRefs(a)` is refused: it is `a`.
4. **The binding is a ref.** The merged binding is declared as `SymbolKind::Ref`, so it can be used
   anywhere a ref can, including as another `useMergedRefs` operand.
5. **The generated base assigns every owner.** For a ref with aliases, `emit_ref_assignments` writes
   `this-><primary> = node;` and then `this-><alias> = node;` for each alias, from the same node
   pointer.
6. **The assignment happens at attach, not in an effect.** The generated base writes the pointer
   during the build walk, so the first layout sees it; an effect would run a frame late.
7. **No runtime state.** The hook adds no field, no behavior and no output. It changes one assignment
   into several.

## 4. Accessibility

None directly: a ref is a pointer, not a semantic. The hook exists so a component can forward a ref
*and* keep one, which is what lets a composite expose its inner control to a parent that needs to
focus it. A component that drops the forwarded ref to keep its own breaks that.

## 5. States

| State | Generated assignment |
|---|---|
| ordinary `useRef` | `this->r = node;` |
| merged ref, primary | `this->inner = node;` |
| merged ref, alias | `this->forwarded = node;` |

## 6. Verification map

| Rule | Test |
|---|---|
| 1 | `test_merged_refs` (two entries, not three) |
| 2 | `test_merged_refs_rejects_a_non_ref` |
| 3 | the analyzer's `call.operands.size() >= 3` check |
| 4 | `test_merged_refs` (the merged binding is a ref) |
| 5 | `test_merged_ref_assignment` |
| 6 | `test_merged_ref_assignment` (the assignment is in the build body) |
| 7 | `test_merged_refs` (no behavior, no output) |

## 7. What this hook does not do

- It does not create a callback ref. A merged ref is a plain pointer assignment; a callback ref would
  need a call site the generated base does not have.
- It does not merge a ref with a callback. A component that needs to run code on assignment writes an
  effect.
- It does not deduplicate. `useMergedRefs(a, a)` assigns `a` twice, which is harmless and simpler than
  a check.
- It does not work across components. The aliases are refs of the *same* component; a parent's ref is
  reached by passing it as a prop, which the JSX subset already supports.
