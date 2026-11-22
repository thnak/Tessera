# Custom hooks v1

Status: draft, normative once accepted. Specifies the custom hooks of
[ADR-065](../../decisions/ADR-065-custom-hooks-by-compile-time-inlining.md): functions named `use...`
that are declared in a project or package and expanded inline at each call site. Terms: a **custom
hook** is such a function; a **call site** is one call of it inside a component or another hook;
**expansion** is the replacement of the call by the hook's statements; the **expanded component** is the
component after every call is expanded, which is what the rules, lowering and code generation see.

## 1. Scope and state of the code

Implemented. `src/compiler/analyzer/src/hook_expansion.cpp` is the pass, `parser_hook.cpp` parses a
declaration, and `FrontEndOptions::custom_hooks` (on by default since #645) selects it; with the
switch off the old `TESSERA_REACT_CUSTOM_HOOK` rejection is what a caller sees. The core hooks, the
library hook manifest and the behavior hooks
([`behavior-hooks-v1.md`](behavior-hooks-v1.md)) are unchanged; a custom hook is built from them.

## 2. Declaration

```tsx
export function usePagination(total: int, pageSize: int = 20) {
  const [page, setPage] = useState<int>(1);
  const hasMore = page * pageSize < total;
  const next = () => setPage(page + 1);
  return { page, setPage, hasMore, next };
}
```

- A function declaration, exported or not, whose name is `use` plus an uppercase letter. Arrow
  functions and function expressions are not hooks in V1 (`TESSERA_REACT_HOOK_FORM`).
- Parameters are positional and typed with a V1 type ([`jsx-subset-v1.md`](jsx-subset-v1.md) section 5)
  or a callback type (`() => void`, `(v: T) => void`). A default is a literal. No destructuring, no
  rest, no `this`.
- The body has the shape of a component body (`jsx-subset-v1.md` section 6): hook calls, handler
  declarations, pure `const` locals and exactly one `return`, which is last. The return is not JSX
  (`TESSERA_REACT_HOOK_RETURN`).
- The return is one of: a pure expression; an array literal `[a, b]`; an object literal
  `{ a, b: expr }`. Each element is a pure expression over the hook's parameters, state, refs,
  locals and handlers. A handler name or a ref name may be returned; any other function is not a value
  (`TESSERA_REACT_HOOK_RETURN`).
- A hook is declared in the same file as its caller, in another project file imported with a relative
  path, or in a package. Imports and re-exports follow the rules components use
  ([`project-config-v1.md`](project-config-v1.md), `lower_project`).

## 3. Call site

```tsx
const p = usePagination(rows.length);          // bound as a whole: p.page, p.hasMore
const [open, toggle] = useToggle(false);      // array pattern for an array return
const { page, next: goNext } = usePagination(total, 10);   // object pattern, with rename
useAnalytics("orders");                        // a hook with no return, as a statement
```

- The call is at the top level of a component body or of another hook, unconditional, in the order of
  the other hooks (`TESSERA_REACT_HOOK_ORDER`, `jsx-subset-v1.md` section 7); the placement checks run on
  the expanded component.
- Arguments: a value parameter takes a pure expression; a callback parameter takes a setter-call arrow,
  or the name of a declared handler or native handler (`jsx-subset-v1.md` section 9). The count and types
  are checked against the declaration (`TESSERA_REACT_HOOK_ARG`).
- A whole-value binding `p` may be used only as `p.field` for an object return, `p` for a single-value
  return, and `p[0]`-style access is not allowed (use the array pattern). Passing `p` itself as a value is
  `TESSERA_REACT_HOOK_RETURN`: there is no record at run time.

## 4. Expansion

Expansion is a pass on the parsed module, before the rules; for each call site, in source order:

1. **Resolve** the callee: same file, import, package. Unresolved is `TESSERA_REACT_UNKNOWN_HOOK`.
2. **Instantiate** a copy of the hook's body statements and give the call site an ordinal `n`, counting
   calls of that hook in the component from 1 in source order (nested calls count where they appear
   after their enclosing expansion).
3. **Rename** every name the hook declares (state, setter, ref, animated value, handler, local, and the
   ids of native handlers and effects) to `<hook>_<n>__<name>`, for example `usePagination_1__page`.
   Names that are not declared by the hook (types, global constants) are untouched.
4. **Bind parameters.** A value parameter whose argument is a plain name (a prop, state, animated
   value or an earlier hook's output) is replaced by that name. Otherwise it becomes a pure `const`
   local `<hook>_<n>__<param> = <argument>`, re-evaluated on each update. A callback parameter is
   replaced by the handler reference.
5. **Splice** the renamed statements at the call position, replacing the call statement.
6. **Bind results.** The call-site pattern becomes a substitution: each bound name, or each `p.field`,
   is replaced everywhere by the renamed source of the returned element. A returned handler or ref
   substitutes as that handler or ref.
7. **Repeat** for calls inside the spliced statements (nested hooks), to a depth of 8.

The result is checked by the unchanged rules, lowering and code generation, as if the author had written
the expanded component. No custom hook reaches the IR.

Consequences the expansion fixes:

- **State is per call site.** Two calls give two states, effects and refs.
- **Effect dependencies** must be names of props, state or animated values (`jsx-subset-v1.md`
  section 7). A parameter used in a dependency list must therefore be passed a plain name;
  an expression argument is `TESSERA_REACT_HOOK_DEP` with the hint to bind it to a `const` first.
- **Native handlers.** A `// Note: bind::<id>` handler or an effect inside a hook has the id
  `bind::<hook>_<n>__<id>` after expansion, which is also the name of the developer-class member
  ([ADR-040](../../decisions/ADR-040-generated-base-and-developer-class.md)). The id is stable under a
  rename of the call-site variable and changes only when the call order of that hook changes.
- **Refs and behavior targets.** A `useRef` in a hook may be returned and attached as `ref={x.ref}` in the
  component; a behavior hook inside the hook that targets it is lowered with the component, and
  `TESSERA_BEHAVIOR_TARGET_UNUSED` is reported on the expanded component if the ref is never attached.

## 5. Typing

Parameter types are declared. The type of a returned element is the type of its expression; an optional
return type annotation on the function is checked against it (`TESSERA_REACT_HOOK_RETURN` on mismatch).
A tuple or object return that mixes types is allowed: each element keeps its own type.

## 6. Library hooks inside a custom hook

| Hook inside | Allowed | Rule |
|---|---|---|
| Core (`useState`, `useRef`, `useAnimatedValue`, `useEffect`) | yes | Renamed per call site |
| Library `environment` hooks | yes | The environment entry is per call site |
| Behavior hooks | yes | The behavior and its output ids are per call site |
| Other custom hooks | yes | Depth 8; cycles are `TESSERA_REACT_HOOK_CYCLE` |
| `useService<T>()` | not in V1 | `TESSERA_REACT_HOOK_SERVICE`: two expansions would require one interface twice, which the code generator refuses; a service is read in the component and passed as an argument. Open item |

## 7. Limits

| Limit | Value | Diagnostic |
|---|---|---|
| Expansion depth | 8 | `TESSERA_REACT_HOOK_DEPTH` |
| Statements added to one component by expansion | 400 (a warning at 200) | `TESSERA_REACT_HOOK_SIZE` |

A hook that is used by no component is still parsed and checked once on its own, with its parameters
typed as declared, so an error is reported without a call site.

## 8. Source mapping and tooling

- Spans of expanded statements keep the position in the hook's own file. A diagnostic whose primary span
  is inside a hook carries the call site as its related span (`RelatedSpan` of the analyzer's
  `Diagnostic`), with the text "expanded here, from <hook>_<n>".
- The editor typings generator (`jsx-subset-v1.md` section 16.2) emits a declaration for each exported
  custom hook of the project, so completion and signature help work in Studio.
- The preview carries state across edits by id; since expanded ids include the hook name and the ordinal,
  state survives edits that do not reorder or rename hook calls.

## 9. Diagnostics

All are errors unless stated; new codes are registered with a fixture in `diagnostic_registry.hpp` and
`tests/golden/jsx/diagnostics-registry.txt`.

| Code | When |
|---|---|
| `TESSERA_REACT_CUSTOM_HOOK` | Kept only for forms this spec does not accept (arrow or expression hooks are `HOOK_FORM`; see below) |
| `TESSERA_REACT_HOOK_FORM` | A hook declaration is not in the form of section 2 |
| `TESSERA_REACT_HOOK_RETURN` | A return form not allowed by section 2, a whole-value use of `p`, or a return type mismatch |
| `TESSERA_REACT_HOOK_ARG` | Argument count or type does not match the declaration |
| `TESSERA_REACT_HOOK_DEP` | A parameter used as an effect dependency was not passed a plain name |
| `TESSERA_REACT_HOOK_CYCLE` | A hook calls itself directly or through other hooks |
| `TESSERA_REACT_HOOK_DEPTH` | Expansion deeper than 8 |
| `TESSERA_REACT_HOOK_SIZE` | Expansion adds more than 400 statements (a warning from 200) |
| `TESSERA_REACT_HOOK_SERVICE` | `useService` inside a custom hook |
| `TESSERA_REACT_HOOK_ORDER` | Unchanged; checked on the expanded component |

## 10. Versioning

The front end changes, the IR does not: a project that uses custom hooks produces IR of the same version
and shape. The editor typings and the diagnostic registry gain entries. Accepting a form not listed here is
a change to this spec, the expansion pass and the conformance tests together.

## 11. Verification

| Layer | What it proves | Where |
|---|---|---|
| Expansion | Each rule of section 4, including renaming, parameter binding and result substitution | `tests/unit/` expansion tests |
| Differential | A component using a hook lowers to the same IR (after id mapping) as the hand-expanded component | a fixture pair per rule |
| Rules | Each diagnostic of section 9 with accept and reject cases, and the hook order on the expanded component | `tests/unit/rules_hooks_tests.cpp` |
| Spans | The related span points at the call site | diagnostics tests |
| Codegen | Generated member names for two calls of one hook; golden of a component with a hook | `tests/golden/codegen/` |
| Preview | State carries across an edit that keeps the call order | scene session tests |

## 12. Open items

- `useService` inside a hook (section 6): either deduplicate service requirements by interface in the IR
  and code generator, or keep rejecting.
- Whether arrow-function hooks (`const useX = () => {}`) are accepted later; React code uses both forms.
- The size limits of section 7 are provisional and need a measurement on real components.
- Hooks that return a handler declared with a native body: the developer-class member name is stable only
  while the call order of that hook is.
