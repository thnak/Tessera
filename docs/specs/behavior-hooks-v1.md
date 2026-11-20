# Behavior hooks v1

Status: draft, normative once accepted. Specifies the `behavior` hook kind of
[ADR-064](../../decisions/ADR-064-astryx-style-component-system.md) item 7; the rationale and the work
order are in [`behavior-hooks.md`](../design/behavior-hooks.md). Terms: a **behavior** is an engine
class that owns focus, keyboard, press, scroll or overflow logic for one element; a **behavior hook** is
the TSX call that creates one; an **instance** is one behavior created for one component instance; an
**output** is a value the behavior reports back; the **registry** is the engine's table of behaviors.

## 1. Scope and what already exists

This spec adds one lowering kind to the library hook manifest of
[`jsx-subset-v1.md`](jsx-subset-v1.md) section 7.1, one IR list to
[`ir-v1.md`](ir-v1.md) section 2 and the rules between them. It does not change the core hooks, the
handler forms of `jsx-subset-v1.md` section 9 or the expression grammar.

State of the code at the time of writing, which this spec must not hide:

- The front end (`src/compiler/rules/src/hooks.cpp`) recognizes only `useState`, `useRef`,
  `useAnimatedValue` and `useEffect`; every other `use*` is `TESSERA_REACT_UNKNOWN_HOOK` or
  `TESSERA_REACT_UNSUPPORTED_API`.
- The built-in hook manifest (section 7.1 of `jsx-subset-v1.md`, file
  `src/compiler/frontend/hooks-manifest.json`) is specified and not implemented: the file and the
  front-end reader do not exist. `environment` lowering and `useService` exist in the IR and in code
  generation (`emit_services.cpp` is the precedent for section 9) but nothing in the front end produces
  them from TSX.
- Implementing this spec therefore includes the generic manifest reader and the library-hook path
  (parse, check, lower) that `environment` hooks also need. Section 3 states it once for both kinds.

## 2. Call form

A behavior hook call is one `const` binding of the whole value at the top level of the component body,
before the `return`, under the placement and order rules of every hook (`TESSERA_REACT_HOOK_ORDER`):

```tsx
const trap = useFocusTrap(trapRef, { active: open, onEscape: () => setOpen(false) });
```

- The leading arguments are the **targets**, in the manifest's order: each is the name of a `useRef` of an
  element type the hook allows for that position (a Popover hook takes `trigger` and `floating`).
  Every manifest target is required; there is no optional target.
- The last argument is an **options** object literal, optional when every option has a default.
- A hook with no outputs may be called as a bare statement `useFocusTrap(...);`; one with outputs must
  bind them (an unused binding is allowed, so a hook never changes shape by use).
- No destructuring, no spread, no computed keys, no more arguments. The same hook called twice with the
  same first target is `TESSERA_BEHAVIOR_DUPLICATE` when the hook is exclusive (section 7).

## 3. Manifest entry

The manifest `schemaVersion` becomes `2`; every version-1 entry stays valid under 2 (a reader of
version 2 accepts both, a reader of version 1 rejects 2 with `TESSERA_JSX_MANIFEST_VERSION`, which is
the existing refusal rule). A `behavior` entry:

| Field | Type | Meaning |
|---|---|---|
| `name` | string | `use` plus an uppercase letter; unique across all hooks; not a core hook name |
| `params` | array | One or more `ref` parameters first, then at most one `options` parameter |
| `ref` parameter | `{ "name", "kind": "ref", "element": <intrinsic name or "any"> }` | A target and the element types it accepts; names are unique |
| `options` parameter | `{ "name", "kind": "options", "fields": [Field] }` | The options object |
| `Field` (value) | `{ "name", "ts", "ir", "default"? }` | A reactive option. `ts` is a type of [`jsx-subset-v1.md`](jsx-subset-v1.md) section 5, `ir` an IR type; a closed token type is `ts: "\"a\" \| \"b\""`, `ir: "string"` |
| `Field` (callback) | `{ "name", "kind": "handler", "payload": [{ "name", "type" }] }` | A callback; `payload` is the IR types the engine passes, in order |
| `returns` | `{ "fields": [{ "name", "ts", "ir" }] }` | The outputs; omitted for a hook with none |
| `lowering` | `{ "kind": "behavior", "behavior": <registry name>, "exclusive": bool }` | `behavior` names a registry entry; `exclusive` defaults to `false` |

Rules for the manifest itself (a build-tool error, never a user diagnostic): every `behavior` value is
in the registry; every `ir` is a V1 type (`bool`, `int`, `float`, `string`, `length`, `color`,
`optional<T>`); field names are unique and lower camel case; a `handler` payload type is a V1 type. A
unit test loads the manifest and the registry and fails on any mismatch, so the two cannot drift.

Who may declare: only the framework's built-in manifest (the file named in section 1). A project or a
package cannot add entries, and `TESSERA_REACT_CUSTOM_HOOK` is unchanged. Package-declared hooks, for
the component hooks of ADR-064 item 7, are a separate decision and are not specified here.

## 4. Arguments

| Argument | Accepted | Evaluated |
|---|---|---|
| Target | An identifier bound by `useRef<T>(null)` where `T` is allowed by that parameter's `element`; two targets of one call must be different refs | Once, at attach |
| Value option | A pure expression ([`jsx-subset-v1.md`](jsx-subset-v1.md) section 8) of the declared type; an omitted option takes `default`; an option with no default is required | On every component update |
| Callback option | A setter-call arrow (section 9.2) or the name of a declared handler or native handler (section 9.3) | Once, at build; the callback is invoked, never re-created |

- A value that is not of the declared type, an extra or unknown option, a missing required option, a
  non-literal where a closed token is declared and an expression that reads an output of the same hook
  are `TESSERA_REACT_HOOK_ARG` (the message names the option and the expected type).
- A callback payload parameter is typed from the manifest; an arrow may declare fewer parameters than
  the payload, never more.
- A value option may read props, state, animated values, other hooks' outputs declared earlier, and
  locals; hooks declared later cannot be read (declaration order, as for state).

## 5. Outputs

- Each output is a read-only value of its declared type, readable in any pure expression, in JSX and as
  an effect dependency (the dependency rules of `component-model-v1.md` section 2 apply as for state).
- A reference id of an output is `beh::<name>.<field>`, where `<name>` is the local name the hook call
  was bound to and `<field>` the output name (dotted, as a record field is). A hook called as a bare
  statement is named `beh::<hook>_<n>` with `n` counting from 1.
- When the engine changes an output the component is marked updated, exactly as for an `environment`
  value ([`component-model-v1.md`](component-model-v1.md) section 5). Several changes inside one input
  dispatch cause at most one update.
- Before the first attach, and after the target is detached, an output has the value its manifest field
  declares as its `default`, or the IR zero value (`false`, `0`, `""`) when none.

## 6. IR

The component gains an optional `behaviors` array ([`ir-v1.md`](ir-v1.md) section 2; omitted when
empty, so a component without behaviors writes the bytes it wrote before and every committed IR golden is
unchanged; additive, `irVersion` stays `1`):

```json
{ "id": "beh::trap", "hook": "useFocusTrap", "behavior": "focus_trap", "exclusive": true,
  "targets": [ "ref::trapRef" ],
  "args": [
    { "name": "active", "expr": { "kind": "ref", "id": "state::open" } },
    { "name": "restoreFocus", "expr": { "kind": "literal", "value": true } },
    { "name": "onEscape", "handler": "handler::1" } ],
  "outputs": [ { "name": "active", "type": "bool" } ],
  "source": { "file": "ui/Dialog.tsx", "offset": 310, "length": 96, "line": 14, "column": 3, "endLine": 14, "endColumn": 99 } }
```

- `args` lists every manifest field in manifest order, including defaulted ones (written as literals), so
  a consumer needs no manifest to evaluate a behavior.
- A callback arg holds a `handler` id (`handler::N` for an inline setter handler, or a `bind::` id for a
  native handler, [`ir-v1-expr.md`](ir-v1-expr.md) section 6).
- Ids: `beh::` entries are unique, in declaration order. The reference ids section of `ir-v1.md` gains
  `beh::<name>.<field>`.
- Determinism ([`ir-v1-expr.md`](ir-v1-expr.md) section 11): arrays keep declaration order; `exclusive`
  is copied from the manifest so the validator needs no manifest either.

Validation (section 9 of `ir-v1-expr.md` gains these hard errors): a `beh::` id that is duplicated; a
`targets` entry that is not a `ref::` of the component; an arg name that is duplicated; an expression that
does not type-check against its field type; a `handler` that does not resolve; a `behavior` name unknown
to the registry (`TESSERA_BEHAVIOR_UNKNOWN`, at generation time, not in the IR validator).

## 7. Diagnostics

All are errors. New codes are registered in `diagnostic_registry.hpp` with a fixture, and
`tests/golden/jsx/diagnostics-registry.txt` is updated in the same change.

| Code | When |
|---|---|
| `TESSERA_REACT_HOOK_ARG` | An argument is not of the declared kind or type (section 4) |
| `TESSERA_BEHAVIOR_TARGET_KIND` | A target ref's element type is not allowed by the manifest for that position |
| `TESSERA_BEHAVIOR_TARGET_UNUSED` | A target ref is not attached to any element of the component's output |
| `TESSERA_BEHAVIOR_DUPLICATE` | Two calls of an exclusive hook share their first target |
| `TESSERA_BEHAVIOR_UNKNOWN` | The manifest names a behavior the registry lacks (build tooling, not a source error) |

## 8. Runtime contract

A behavior implements the engine interface of `include/tessera/ui/behavior/behavior.hpp`:

| Call | Contract |
|---|---|
| `attach(Targets, Args)` | Called when **every** target ref holds a node (one node per target, in manifest order). May allocate. Registers listeners with the focus, input and accessibility services. May not invoke a callback. |
| `update(Args)` | Called when the hash of the evaluated value options changed (section 9). No allocation. May not invoke a callback. |
| `detach()` | Called before the target node is destroyed, when the ref becomes empty, and at unmount. Removes every registration. Idempotent. |
| `outputs()` | Returns the current outputs; read by the component after each dispatch and update. |

Lifecycle rules:

1. **Order.** Attach in declaration order; detach in reverse. Two behaviors on one element never see
   each other.
2. **Conditional target.** When a target element is a conditional child, the behavior attaches when
   all its refs are non-null and detaches as soon as any becomes null; outputs return to their
   defaults. A behavior is therefore never attached to a destroyed node, and never half-attached.
3. **Callbacks** run synchronously inside the input dispatch that caused them, on the UI thread, with
   the normalized payload; a callback never runs inside `attach` or `update` (a state change made there
   is deferred to the next dispatch).
4. **Time.** A behavior reads the clock only through the runtime clock interface
   (`src/runtime/clock.*`), so a `ManualClock` test is deterministic. No wall clock, no threads.
5. **Memory.** Instances live in bounded slots counted from the IR (as `useEffect` slots,
   `component_lifecycle.hpp`). Mount, update and unmount of a component allocate nothing after the
   first attach.
6. **Platform.** A behavior reaches the platform only through the HAL services (focus, clipboard,
   accessibility bridge); it never sees a window handle or swapchain (ADR-020).
7. **Reduced motion and platform profile** (ADR-034) are read from the engine services, never from
   behavior arguments.

## 9. Generated code

Code generation follows the `useService` precedent (`emit_services.cpp`): emission is conditional on
`component.behaviors` being non-empty, so every committed `.g.hpp` golden of a component without
behaviors stays byte-identical. For a component with behaviors the generated base has:

- one private member per behavior of type `tessera::ui::behavior::Instance`, named from its id;
- in `build()`: attach after the target node exists, in declaration order; in the update pass: evaluate
  the value options, hash them as effect dependencies are hashed, and call `update` only on change;
- callback options as the thunks `bind::` handlers use, so a callback needs no new mechanism;
- in `unmount()`: detach in reverse order;
- one read accessor per output, used by the expressions that reference it. No accessor for an instance
  is public: the developer class derived from the base sees none of it (ADR-040 item 6). A TSX
  binding that collides with a generated behavior member is the usual `TESSERA_CODEGEN_UNSUPPORTED`.

## 10. Preview

The design-time preview builds scenes from IR with a seeded scope (`tools/codegen/scene_build.cpp`) and
dispatches handlers into it (`scene_session.cpp`); today it holds no live component instance. For a
behavior to run there, the preview session must create the same registry `Instance` from the IR entry
and drive it with the session's normalized input, in the native preview and in the Wasm canvas, with the
same lifecycle rules (section 8). Until then the preview shows the component's static result: outputs
at their defaults and no behavior running. This is stated in the preview's capabilities so a user is not
misled.

## 11. Versioning

- The manifest `schemaVersion` is `2` when it contains a `behavior` entry (section 3).
- `behaviors` is additive: `irVersion` stays `1`. A V1 consumer that does not know the field would
  reject a module that has it; the compiler, the evaluator and the code generator ship together, so
  that is the accepted risk, as for `types` and `record` ([`ir-v1-expr.md`](ir-v1-expr.md) section 12).
- Adding a behavior hook, changing a signature, an output or a default is a change to the manifest, the
  registry and the hook's own spec together (section 12).
- Versions are tabulated in [`versioning-v1.md`](versioning-v1.md).

## 12. Per-hook spec and tests

Each library hook has its own contract spec (name, arguments, guarantees, accessibility; the format of
the component spec template, sections 4, 5 and 8, in `docs/specs/components/_template.md` once it lands), and tests at these layers:

| Layer | What it proves | Where |
|---|---|---|
| Behavior | Every rule of the hook's spec, with a scene, the manual clock and injected input | `tests/unit/behavior_<name>_tests.cpp` |
| Manifest | The manifest entry is valid and the registry has it | one shared test |
| Analyzer | Accept and reject tables for each section 7 code | next to `lowering_hooks_*` |
| IR | Lowering output and the JSON writer, with `tests/golden/ir/` | next to the IR goldens |
| Codegen | The generated base for a component with behaviors (new golden), and byte identity for one without | `tests/golden/codegen/` |
| Preview | The session drives the instance and outputs update (once section 10 is implemented) | `tests/unit/` scene session tests |

## 13. Open items

- Opaque handle outputs and package-declared hooks are not part of V1: the Table plugins of ADR-064
  item 8 are typed props, not hooks that return a handle ([`behavior-hooks.md`](../design/behavior-hooks.md)
  sections 8 and 10). Both can be added later without changing this spec's entries.
