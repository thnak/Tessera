# Behavior hooks: how a hook becomes engine code

Status: design proposal for [ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7.
Nothing here is implemented. It fixes the foundation the library hooks (`useFocusTrap`,
`useListFocus`, `usePressFeedback`, ...) stand on, so that no component has to work around a missing
compiler feature. Terms: a **behavior** is an engine class that owns focus, keyboard, press or
overflow logic for one element; a **behavior hook** is the TSX call that creates one; an **instance** is
one behavior created for one component instance.

## 1. What exists and why it is not enough

- The compiler accepts the core hooks (`useState`, `useRef`, `useAnimatedValue`, `useEffect`) and the
  hooks of the library manifest ([`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 7.1). A
  manifest hook has one lowering kind, `environment`: no parameters, a read-only value (or a value and
  a setter) from an engine provider.
- A hook declared in the project is rejected (`TESSERA_REACT_CUSTOM_HOOK`).
- Code generation does not emit library hooks yet: `emit_component.cpp` reports that `environment` hooks "are a later slice". The behavior kind therefore also finishes the generic library-hook path in codegen, not only adds a kind.
- The behavior primitives of `packages/tessera-ui/primitives/` (Press, FocusScope, RovingFocus,
  Toggle, ...) are TSX **components**, each drawing nothing, wrapped around their children.

A behavior needs what none of these gives: a target element (a `ref`), reactive arguments, callbacks, and
values it reports back (`pressed`, the active index, the overflow count). Wrapper components cannot
express "this hook acts on that existing element" and add a node to every use, which changes layout and
the accessibility tree. They also hold their logic in generated TSX, where it cannot be unit-tested
without a compiler run. The decision is to build the missing hook kind, not to extend the wrappers.

## 2. The model

```tsx
const trapRef = useRef<View>(null);
const [open, setOpen] = useState<boolean>(false);

const trap = useFocusTrap(trapRef, {
  active: open,
  restoreFocus: true,
  onEscape: () => setOpen(false),
});

return <View ref={trapRef} aria-modal={open}>{...}</View>;
```

A behavior hook call is one `const` binding at the top level of the component body, under the same
placement and order rules as every hook (`TESSERA_REACT_HOOK_ORDER`). Its arguments are:

| Argument kind | Accepted form | Reactive |
|---|---|---|
| Target | a `useRef` name of a declared element type, first position | n/a |
| Option value | a pure expression of boolean, number, string or a closed token | yes: read on every update |
| Callback | a handler form of [`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 9: a setter-call arrow or a `bind::` native handler | no: fixed at build |

The options are one object literal. No spread, no computed keys, no nested objects other than a closed
list of tokens, so the analyzer can check every argument against the manifest. The return value, when
the hook has one, is a record of read-only fields (`trap.active`, `press.pressed`): each is a
**behavior output**, usable in any pure expression and in JSX, and it behaves like state: when the
engine changes it, the component is marked updated (as for an environment value,
[`component-model-v1.md`](../specs/component-model-v1.md) section 5).

## 3. The manifest entry

A new lowering kind `behavior` joins `environment` in the library hook manifest:

```json
{
  "name": "useFocusTrap",
  "params": [
    { "name": "target", "kind": "ref", "element": "any" },
    { "name": "options", "kind": "options", "fields": [
        { "name": "active", "ts": "boolean", "ir": "bool", "default": true },
        { "name": "restoreFocus", "ts": "boolean", "ir": "bool", "default": true },
        { "name": "onEscape", "kind": "handler", "payload": [] }
    ] }
  ],
  "returns": { "fields": [ { "name": "active", "ts": "boolean", "ir": "bool" } ] },
  "lowering": { "kind": "behavior", "behavior": "focus_trap" }
}
```

`behavior` names an entry of the engine's behavior registry (section 5). The manifest is the single
place that says what a hook accepts and returns; the analyzer, lowering, code generator, preview
evaluator and documentation all read it. A manifest that names a behavior the registry lacks fails a
unit test, so the two cannot drift.

Who may declare a behavior hook: the framework's built-in hook manifest only
([`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 7.1; a project cannot add entries). That
manifest is specified but not yet implemented in the front end; see
[`behavior-hooks-v1.md`](../specs/behavior-hooks-v1.md) section 1. This keeps the custom-hook rule of
ADR-036 intact for applications. The component hooks of ADR-064 item 7 (`useTable*`) need
package-declared hooks, which is a separate decision.

## 4. IR

The component gains a `behaviors` list ([`ir-v1.md`](../specs/ir-v1.md) section 2; additive, so IR stays
v1 and older consumers ignore it):

```json
{ "id": "beh::trap", "hook": "useFocusTrap", "behavior": "focus_trap",
  "target": "ref::trapRef",
  "args": [ { "name": "active", "expr": { "ref": "state::open" } },
            { "name": "restoreFocus", "expr": { "literal": true } },
            { "name": "onEscape", "handler": "handler::1" } ],
  "outputs": [ { "name": "active", "type": "bool" } ] }
```

Every use of `trap.active` lowers to a `ref` to `beh::trap.active`. Ids follow the stable rules of the IR
(declaration order, `beh::<name>`). A behavior whose target ref is never attached to an element is a
compile error (`TESSERA_BEHAVIOR_TARGET_UNUSED`), because the behavior would silently do nothing.

## 5. Engine side

Behaviors live in the engine, not in generated code:

| Part | Place | Role |
|---|---|---|
| Interface | `include/tessera/ui/behavior/behavior.hpp` | `attach(Node&)`, `update(ArgsView)`, `detach()`, `outputs()`; no allocation after `attach`. |
| Registry | `src/ui/behavior/registry.cpp` | name -> factory; also the list the manifest test checks. |
| Implementations | `src/ui/behavior/<name>.cpp` | `focus_trap`, `roving_focus`, `typeahead`, `hotkeys`, `press_feedback`, `scroll_lock`, `overflow`, `announce`, `clipboard`, `interactive_role`. One file per behavior, under the file-size limit. |
| Shared services | the existing focus, input dispatch, a11y bridge and layout code | A behavior calls these; it never reaches platform handles (ADR-020). |

Each behavior is deterministic and testable headlessly through the scene and the manual clock: a unit
test builds a scene, attaches the behavior, injects normalized input and asserts focus, outputs and
callbacks. This is the only place its rules are verified; code generation and the preview are tested for
wiring, not for behavior.

Instances are held in bounded slots, like `useEffect` state
(`include/tessera/ui/scene/component_lifecycle.hpp`): the slot count comes from the IR, so mounting,
updating and unmounting allocate nothing.

## 6. Generated code and the preview

There are two consumers of the IR that must agree: native code generation and the design-time preview
(`tools/codegen/scene_build.cpp`, which builds scenes from IR for Studio and the Wasm canvas). A behavior
must not be implemented twice, so both only **wire** the same engine class:

- **Generated base** (ADR-040): one private member per behavior, `tessera::ui::behavior::Instance
  beh_trap_;`. `build()` attaches it after the target node exists; the update pass passes the current
  option values (a hash decides whether anything changed, as for effects); callbacks are the same
  thunks `bind::` handlers use; `unmount()` detaches in reverse declaration order. Outputs have
  accessors the generated code reads; the developer class sees none of it (ADR-040 item 6).
  Emission lives in a new `src/compiler/codegen/src/emit_behaviors.cpp`.
- **Preview**: `tools/codegen/scene_build.cpp` builds the scene from IR with a seeded scope, and
  `scene_session.cpp` dispatches handlers into it; there is no live component instance there today.
  A behavior needs one, so the preview session must create the same `Instance` from the IR entry and
  the same registry, and drive it with the session's input. This is a work item of its own (section 9,
  step 4), not a free consequence, and until it lands Studio shows a behavior's static result only.
- **Dirty rules**: a changed output marks the component updated through the existing update scheduler;
  no behavior touches the scene directly outside `attach`/`update`/`detach`.

## 7. Diagnostics

| Code | When |
|---|---|
| `TESSERA_REACT_HOOK_ARG` | An argument is not of the declared kind (a non-ref target, a non-literal token, a spread). |
| `TESSERA_BEHAVIOR_TARGET_UNUSED` | The target ref is not attached to an element. |
| `TESSERA_BEHAVIOR_TARGET_KIND` | The ref's element type is not allowed for this behavior. |
| `TESSERA_BEHAVIOR_DUPLICATE` | Two behaviors of an exclusive kind (two focus traps) target one element. |
| `TESSERA_BEHAVIOR_UNKNOWN` | The manifest names a behavior the registry lacks (a build-tool error, not a user error). |

## 8. Hooks that do not fit

Not every library hook should be a behavior:

- `useMediaQuery` is an `environment` hook (a window property), like `useBreakpoint`; it extends the
  existing provider list.
- `useMergedRefs` has no use in this model: a component forwards no ref, and an element has one `ref`.
  It is dropped from the core set unless a case appears that a behavior cannot cover.
- `useAnnounce` and `useClipboard` are services (ADR-040 `useService`): an injected interface with a
  production and a test implementation. They return a handle, not behavior outputs.
- A hook that returns an opaque value for a component to consume (the Table plugins of ADR-064 item 8)
  needs an **opaque handle** type in the IR (`handle<TablePlugin>`). It is deferred to the Table
  design; this document does not decide it.

## 9. Order of work

1. This design reviewed, then a normative spec `docs/specs/behavior-hooks-v1.md` (the manifest `behavior`
   kind, the IR `behaviors` list, diagnostics); `jsx-subset-v1.md` and `ir-v1.md` are near their size
   limit, so they link the new spec instead of growing.
2. Analyzer rules and lowering: manifest parsing, argument checking, `behaviors` in the IR.
3. The engine interface, registry and the first behavior (`press_feedback`: smallest, needs the
   pressed and hovered outputs and one target), with its headless test.
4. Code generation (`emit_behaviors.cpp`) and the preview wiring, with a golden of the generated base.
5. The remaining core behaviors, one issue each (#624 to #635 adjusted by section 8).
6. Move the existing primitives (`Press`, `FocusScope`, `RovingFocus`, `Toggle`, ...) onto the behavior
   hooks, delete the wrapper versions and update `Button` to call the hook. No wrapper component is kept
   as an interim form.

## 10. Decisions

Owner, 2026-11-20 ("follow the proposals"):

- The first behavior is `press_feedback`: the smallest one that needs a target, an output pair
  (`pressed`, `hovered`) and the full path through the manifest, IR, engine, code generation and
  preview. `focus_trap` follows, because Dialog needs it.
- An element may carry two behaviors of different kinds. Exclusive kinds (two focus traps on one
  element) are rejected with `TESSERA_BEHAVIOR_DUPLICATE`.
- A behavior output may be used as an effect dependency, as state can.
