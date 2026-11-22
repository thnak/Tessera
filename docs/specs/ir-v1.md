# Tessera IR v1: module, components, nodes and record types

Status: proposed (Big Step C, the compiler vertical slice, defined in
[AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step), section "Big Steps"). Normative for `src/compiler/ir/` and
codegen. "V1" in this file means the first-release scope ([AGENTS.md](../../AGENTS.md#non-negotiables) rule 8, "V1 scope
discipline"); "IR" means the compiler intermediate representation (see the
[glossary](../reference/glossary.md)).

The IR is the only input to C++ codegen. It is neutral, versioned, serializable to JSON, and
independent of the JSX parser
([ADR-007](../../decisions/ADR-007-compiler-ir.md), a neutral IR between JSX and C++ codegen;
the JSX front end is [ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md)).
Codegen must not read the parser AST.

This is **part 1** of the spec: the module, components, handlers, node kinds (including virtual
collections and inline spans), automation IDs, custom elements, text input nodes and component
nodes. Expressions, loops, bindings, spans, validation, codegen mapping, determinism, versioning and
the worked example are in [`ir-v1-expr.md`](ir-v1-expr.md), part 2; section numbers continue across
both parts, so every `section N` reference stays valid.

## 1. Top level

```json
{
  "irVersion": 1,
  "module": {
    "name": "factory",
    "imports": ["std", "tessera"]
  },
  "components": [],
  "bindings": [],
  "stylesheets": []
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `irVersion` | integer | yes | Must be `1` |
| `module.name` | string | yes | Lowercase C++ namespace root |
| `module.imports` | string[] | no | Resolved module ids; no file paths |
| `components` | Component[] | yes | Deterministic order (source path, then name) |
| `bindings` | Binding[] | no | Sorted by id |
| `stylesheets` | StylesheetRef[] | no | One per imported `.css` (`id`, `path`, `hash` = `fnv1a64:` of the file bytes), sorted by path. A `*.module.css` also carries `classes` (section 1.1) |
| `types` | RecordType[] | no | Declared record types, sorted by `name` (section 2.2). Omitted when a module declares no record |

### 1.1 Scoped classes of a CSS module (additive, issue #463)

A `*.module.css` stylesheet ([`css-isolation.md`](../design/css-isolation.md)) carries the class map
its TSX reads, so the inspector can show the authored name and the selector index can see the scoped
one. `classes` is optional and omitted when empty, so a module with no `.module.css` stays
byte-identical to the IR before it (the same rule as `types`, section 2.2); no `ir_version` bump.

```json
"stylesheets": [
  {
    "id": "css::ui/Card.module",
    "path": "ui/Card.module.css",
    "hash": "fnv1a64:...",
    "classes": [
      { "local": "card", "scoped": "card_0b82a5" },
      { "local": "title", "scoped": "title_0b82a5" }
    ]
  }
]
```

`classes` is sorted by `local` and unique. `scoped` is `<local>_<h>`, where `<h>` is 6 lowercase hex
digits of FNV-1a 64 over `path`; the compiler computes it, so the native CLI and the Wasm preview
agree. `hash` stays the hash of the file bytes — it identifies the source file, not the rewrite.

## 2. Component

```json
{
  "id": "comp::Counter", "componentId": "1234567890123456789", "name": "Counter",
  "source": { "file": "ui/Counter.tsx", "offset": 0, "length": 310, "line": 1, "column": 1, "endLine": 14, "endColumn": 2 },
  "props": [{ "name": "initial", "type": "int", "default": 0 }],
  "state": [{ "id": "state::count", "name": "count", "type": "int", "initial": 0 }],
  "effects": [{ "id": "effect::sync", "deps": ["state::count"], "handler": "bind::on_count_changed", "cleanup": "bind::on_count_changed_cleanup" }],
  "refs": [{ "id": "ref::label", "name": "label", "target": "node::2" }],
  "animatedValues": [{ "id": "anim::opacity", "name": "opacity", "type": "float", "initial": 1.0 }],
  "root": { "kind": "View", "id": "node::1", "uiId": "111111111111111111",
    "source": { "file": "ui/Counter.tsx", "offset": 52, "length": 250, "line": 3, "column": 3, "endLine": 13, "endColumn": 9 },
    "children": [] }
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `id` | string | yes | Debug key, unique within the project |
| `componentId` | string | yes | uint64 decimal string; stable automation/trace identity |
| `name` | string | yes | PascalCase component name |
| `source` | SourceSpan | yes | For diagnostics/source maps |
| `props` | Prop[] | no | Typed props |
| `state` | State[] | no | `useState` |
| `handlers` | Handler[] | no | Inline setter handlers (section 2.1) |
| `effects` | Effect[] | no | `useEffect`. An Effect has `id`, `deps`, `handler` (binding id), `source`, and an optional `cleanup` (binding id of the cleanup handler; it runs before the effect runs again and on unmount) |
| `refs` | Ref[] | no | `useRef` |
| `animatedValues` | AnimatedValue[] | no | `useAnimatedValue` |
| `environment` | Environment[] | no | Library hooks with `environment` lowering ([`jsx-subset-v1.md`](jsx-subset-v1.md) section 7.1). An Environment has `id` (`env::<name>`), `name`, `provider` (`theme` or `breakpoint` in V1), `type` (an IR type), optional `writable` (boolean, default `false`) and `source`. A change of the provider's value updates the component like a state change. Only an Environment with `writable: true` may be the target of a setter call in a handler (`useThemeState` in V1) |
| `services` | Service[] | no | ADR-050 section 3.1 (`useService<T>()`). Additive and omitted when empty. A Service has `id` (`svc::<name>`), `name` (the TSX binding the generated accessor is named after), `type` (the C++ service interface, fully qualified, from the project's service manifest), `header` (the header declaring `type`) and `source`. Codegen resolves each entry once at mount; `service<T>()` for a `T` not in this list is `TESSERA_CG_SERVICE_UNDECLARED` |
| `root` | Node | yes | Exactly one root node |

`props[].default` and `state[].initial` are Expressions (section 4): a literal is the bare JSON value,
any other expression its object (`{ "kind": "ref", "id": "prop::start" }`). A `Prop` is `{ "name",
"type", "optional"?, "default"? }`; `optional` is the source `?` (the caller may omit the prop), and
`children` has type `children` and neither. Reference ids are `state::`, `prop::`, `ref::`, `anim::`,
`env::<name>`; a field of a record-typed value is dotted (`item.id`); loop item/index names are as
written. V1 types: `bool`, `int`, `float`, `string`, `color`, `length`, `node_ref`, `list<T>`, `children`
(section 3.4; props only), `optional<T>`, and for props `callback` or a payload record name
(`TextChangeEvent`). A record type name is valid only when it is declared in `types` (section 2.2). No
arbitrary objects/functions in state.

## 2.1 Handlers

An inline setter handler ([`jsx-subset-v1.md`](jsx-subset-v1.md) section 9.2) needs no developer member, so
it is an IR record of its own, referenced from `events`:

```json
{ "id": "handler::1", "params": [{ "name": "e", "type": "string" }],
  "actions": [
    { "kind": "set", "target": "state::count", "value": { "kind": "binary", "op": "+", "left": { "kind": "ref", "id": "state::count" }, "right": { "kind": "literal", "value": 1 } } },
    { "kind": "animSet", "target": "anim::opacity", "value": { "kind": "literal", "value": 0.5 }, "animate": false } ],
  "source": { "file": "ui/Counter.tsx", "offset": 200, "length": 40, "line": 9, "column": 5, "endLine": 9, "endColumn": 45 } }
```

- `id` is `handler::<N>`, numbered from 1 in source order within the component, and unique in it.
- `actions` run in order. `set` writes a state variable through its setter; `animSet` is an
  animated-value `.set(...)`, with `animate` defaulting to `true`. Values are pure Expressions that may
  read `params`. `focus`/`blur` (#226, `r.focus()`/`r.blur()`) take a `ref::` `target`, no `value`, and are no-ops until the ref is assigned: `blur` clears focus only if the node holds it. `callProp` (#286) calls a callback prop of the component: `target` is `prop::<name>` (a `() => void` or payload-record prop), `value` is the handler's payload parameter ref for a payload callback and absent otherwise; an unset callback is skipped. Any action may carry a pure bool `guard` (#236) and runs only when it holds.
- `if` (additive, ADR-066 item 2) is the conditional action: `guard` is the pure bool condition,
  `then` is the list that runs when it holds, and `else` is the list that runs when it does not
  (absent for a bare `if`). It carries no `target` and no `value`. Both lists are action lists, so a
  nested `if` is an `if` inside one of them and a chain needs no new kind. It is how `if`/`else` and
  an early `return;` in a handler body lower:

  ```json
  { "kind": "if", "guard": { "kind": "ref", "id": "prop::disabled" },
    "then": [], "else": [ { "kind": "set", "target": "state::busy", "value": { "kind": "literal", "value": true } } ] }
  ```

  An `if` with no `guard`, or one that carries a `target` or a `value`, is `TESSERA_IR_MALFORMED`.
  The evaluator and the generated C++ run the same branch, which is the differential property the
  ADR asks for.
- An `animSet` may carry literal timing, omitted when absent (default 200 ms `ease-out`): `durationMs`
  (0..60000), `easing` (a CSS keyword or `cubic-bezier(x1, y1, x2, y2)` with x in [0, 1]) or, exclusive with both and with `animate: false`, `spring` `{stiffness, damping, mass}` in (0, 1e5], [0, 1e4], (0, 1000]; invalid timing is `TESSERA_IR_ANIMATION`.
  `onComplete` (additive) is the `handler::N` of a parameterless handler of the component, run once when the value reaches that set's target (a snap completes at once); unknown: `TESSERA_IR_UNRESOLVED`, with parameters or on a `set`: `TESSERA_IR_ANIMATION`.
- A handler declared once and used by several event props is one record with several references.

## 2.2 Record types (additive, ADR-051)

`types` is the module's declared record table ([ADR-051](../../decisions/ADR-051-ir-record-types-and-record-values.md)):
a record is a TypeScript object type with named, typed members (`type Size = { w: int; h: int }`). The
array is **sorted by `name`** for determinism (section 11) and **omitted when empty**, so a module that
declares no record writes exactly the bytes it wrote before this section existed.

```json
"types": [
  { "id": "type::Size", "name": "Size",
    "members": [
      { "name": "w", "type": "int" },
      { "name": "h", "type": "int", "optional": true }
    ],
    "source": { "file": "ui/Card.tsx", "offset": 0, "length": 40, "line": 1, "column": 1,
                "endLine": 1, "endColumn": 41 } }
]
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `id` | string | yes | `type::<name>` |
| `name` | string | yes | PascalCase record name; unique in the module |
| `members` | Member[] | yes (may be empty) | Declaration order is preserved (it is the aggregate-initialization order in generated C++) |
| `members[].name` | string | yes | Unique within the record |
| `members[].type` | string | yes | Any IR type of section 2: a primitive, `list<T>`, `optional<T>`, or another record (or a payload record) by name |
| `members[].optional` | boolean | no | The source `?`: a record value may omit it (its value is empty). Default `false` |
| `source` | SourceSpan | yes | Span of the declaration |

- **Member types.** A member is a value: `callback` and `children` have no record form
  (`TESSERA_IR_UNSUPPORTED`). Every other type of section 2 is allowed, including another record name,
  which is resolved against this table (`TESSERA_IR_UNKNOWN_TYPE` for a name that is not declared).
- **Cycles.** A record may not contain itself, directly or through other records, in any `list<T>` or
  `optional<T>` wrapper: `TESSERA_IR_TYPE_CYCLE`.
- **Identity and duplicates.** The identity is the name, as for components: two entries with the same
  name are `TESSERA_IR_DUPLICATE_ID`, never a merge.
- **Payload records.** The framework's payload records (`TextChangeEvent`, `ClickEvent`, the typed
  event objects of [`jsx-subset-v1.md`](jsx-subset-v1.md) section 16.2) are **predeclared and are not
  listed** in `types`. They are valid type names for a prop or a handler parameter, but they are
  framework data: a `record` value of a payload record is `TESSERA_IR_UNSUPPORTED`, and a dotted
  `ref` into one is not member-checked (section 9.1).
- **Unused record.** A declared record that nothing uses is a **warning** (`TESSERA_IR_UNUSED_TYPE`),
  never an error: the table is additive data a project may keep for a later slice. "Used" means named
  by a component (a prop, state, animated value or environment type, a handler parameter, a binding type
  or a `ref` base) or reachable from such a record through its member types; a record reachable only
  from an unused record stays unused.
- **Additive.** The field is optional and `irVersion` stays `1` (section 12). Lowering collects the
  declared records and emits a `record` value where a record-typed `useState` initial or prop default
  is written (section 4.3).

## 3. Node

```json
{
  "id": "node::2",
  "uiId": "987654321098765432",
  "automationId": null,
  "source": { "file": "ui/Counter.tsx", "offset": 80, "length": 52, "line": 4, "column": 5, "endLine": 4, "endColumn": 57 },
  "kind": "Text",
  "key": null,
  "props": { "className": "label" },
  "style": { "font-size": "14px", "color": "var(--text)" },
  "events": { "onClick": "bind::on_click" },
  "text": { "kind": "template", "parts": ["Count: ", { "kind": "ref", "id": "state::count" }] },
  "children": [],
  "condition": null,
  "loop": null
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `id` | string | yes | Debug key, unique within the component |
| `uiId` | string | yes | uint64 decimal string; stable automation/trace identity |
| `automationId` | string/null | no | Explicit uint64 override; must equal `uiId` |
| `kind` | string | yes | `View`, `Text`, `Button`, `TextInput`, `TextArea`, `Image`, `Path`, `Scroll`, `Spacer`, `VirtualList`, `VirtualGrid`, `ErrorBoundary`, `CustomElement`, `Component`, `Slot` |
| `component` | string | for `Component` | Id of the referenced component (section 3.4) |
| `element` | string | for `CustomElement` | Static element id from `ui/elements.json` |
| `source` | SourceSpan | yes | Span of the node's JSX element (section 8) |
| `key` | string/int/null/Expr | no | Stable list identity; an Expression (section 4) on a node that carries `loop`, a literal otherwise |
| `props` | object | no | Typed intrinsic props |
| `style` | object | no | Style tokens; inline style |
| `events` | object | no | Event name -> binding id (`bind::...`) or inline handler id (`handler::...`, section 2.1) |
| `text` | Expr/null | no | Text content; only a `Text` node has this field. Empty when the node carries inline spans (section 3.6) |
| `children` | Node[] | no | Paint order. A `Text` node's children are its inline spans (section 3.6) |
| `condition` | Expr/null | no | Static conditional; V1 bool expression (never a template or a non-bool literal) |
| `loop` | Loop/null | no | Static list loop; this node is the repeated template (section 5) |
| `itemTemplate` | Node/null | for `VirtualList`/`VirtualGrid` | The static item template of a virtual collection (section 3.5) |

Node kinds are closed in V1; the list above, with `Component` and `Slot` (section 3.4), is the complete list.
A reference to a project or library component is a `Component` node; there is no dynamic
component type. `CustomElement` is an intrinsic native-render node, not a dynamic
component type; see [`custom-elements-v1.md`](custom-elements-v1.md).

`VirtualList` and `VirtualGrid` are collection nodes defined in
[`virtualization-v1.md`](virtualization-v1.md). They carry a data-source ref/list, a static item
template, overscan/estimate/gap, a required item key, and optional sticky headers. They do not
represent VM/container virtualization or a virtual DOM.

## 3.5 Virtual collections

```json
{
  "kind": "VirtualList",
  "id": "node::rows",
  "uiId": "111111111111111111",
  "props": { "estimateSize": 32, "overscan": 200, "gap": 4 },
  "events": { "onEndReached": "bind::on_end_reached" },
  "itemTemplate": {
    "kind": "View",
    "id": "node::row",
    "uiId": "222222222222222222",
    "key": { "kind": "ref", "id": "row.id" },
    "loop": { "kind": "loop", "item": "row", "index": "i", "source": { "kind": "ref", "id": "state::rows" } },
    "style": { "padding": "4px 8px" }
  }
}
```

- `itemTemplate` is one static node subtree. It carries the `loop` (item and index names and the
  data source, a list `ref`) and the item `key` exactly like a section 5 template, but the runtime
  realizes only the visible range plus overscan; the data source is the template's `loop.source`,
  so there is no separate `data` prop.
- **Lowering.** The element takes exactly one child, a `.map` of the section 5 loop form; the
  callback element becomes `itemTemplate` with its `loop` and the key expression of its `key`
  attribute (its item identity). The `.map` receiver is `loop.source` and must be a list `ref`; the
  item and optional index are the template's locals.
- **Size default.** A collection with neither `itemSize` nor `estimateSize` records
  `"estimateSize": 32`, the unmeasured 32 px estimate of
  [`virtualization-v1.md`](virtualization-v1.md) section 4 (no per-item guess, no heuristic).
- A `VirtualList`/`VirtualGrid` has an `itemTemplate` but no `children` and no `loop` of its own; its
  template is static (no `condition`, no dynamic component type) and counts toward the section 5 loop
  depth. Any other kind with `itemTemplate` is `TESSERA_IR_MALFORMED`; a missing/multiple/non-`.map`
  child is `TESSERA_IR_VIRTUAL`; a missing item key is `TESSERA_IR_LIST_KEY`; a non-`ref` `.map`
  source is `TESSERA_IR_UNSUPPORTED` (section 5 loop). The field is additive, so `irVersion` stays `1`;
  the runtime contract is [`virtualization-v1.md`](virtualization-v1.md).

## 3.6 Inline text spans

A `Text` node may carry inline spans instead of one `text` Expression
([`elements-basic.md`](jsx-intrinsics-v1/elements-basic.md) section 2.1); it stays one layout box.

```json
{ "kind": "Text", "id": "node::7", "uiId": "123456789012345678", "style": { "font-size": "14px" },
  "children": [
    { "kind": "Text", "id": "node::8", "uiId": "223456789012345678", "text": { "kind": "literal", "value": "By continuing you agree to the " } },
    { "kind": "Text", "id": "node::9", "uiId": "323456789012345678", "props": { "role": "link" }, "style": { "color": "#0b57d0", "font-weight": "700" },
      "events": { "onClick": "bind::on_terms" }, "text": { "kind": "literal", "value": "Terms" } } ] }
```

- The parent has no `text`; its `children` are all and only its spans, in paint order. A span is a
  `Text` node with a literal `text`, the span attributes of
  [`html-aliases.md`](jsx-intrinsics-v1/html-aliases.md) section 3.6, and no `children`, condition,
  loop, key, fallback, item template, component or element.
- Byte ranges are derived from the concatenated span texts (at most 16 spans and 256 text bytes per
  `Text`). A non-span child, a non-literal span text, a nested span, a disallowed field or a cap
  violation is `TESSERA_IR_SPAN`. Additive (a node already had `children`), so `irVersion` stays `1`
  (section 12); codegen emits each span as a child scene node plus the parent's paint payload.

## 3.1 Automation IDs

- `componentId` and `uiId` (numeric ids of a component and of a node, used by automation and
  traces) are uint64 decimal strings in JSON; `uint64_t` in C++.
- The compiler emits `generated/automation.manifest.json` mapping numeric IDs to debug paths,
  kinds, components, and source spans.
- `automationId` is optional; if set, it must equal `uiId`.
- IDs are deterministic per source/config; collisions are hard errors.
- List/grid instances use `(uiId, itemKey)`; the template keeps one `uiId`.
- Debug `id` strings remain for humans; tools use numeric IDs.
- See [`automation-ids-v1.md`](automation-ids-v1.md).

### 3.1.1 Instance key

A component instance is identified at design time by `(componentId, instanceKey)`. The `instanceKey` is the list item `key` values of the enclosing `loop` nodes, outermost first, each converted to a string, joined with `/`. It is the empty string when the instance is not inside any loop. It is computed from run-time data by the evaluator and by tools; it is not stored in the IR file. A node inside an instance is identified by `(componentId, instanceKey, nodeId)` ([`designer-protocol-v1.md`](designer-protocol-v1.md) section 3.2).

## 3.2 Custom elements

```json
{
  "kind": "CustomElement",
  "id": "node::waterfall",
  "uiId": "444444444444444444",
  "element": "Waterfall3D",
  "props": { "intensity": 0.8, "camera": "main" },
  "events": { "onSelect": "bind::on_select", "onPointer": "bind::on_pointer" },
  "children": []
}
```

- `element` is a static literal resolved against `ui/elements.json`.
- `props`/`events` are validated against the element schema; no unknown fields.
- `CustomElement` is a leaf; `children` must be empty.
- Shaders/pipeline/composition/input/test capabilities come from the element manifest, not JSX.
- No dynamic element expression and no runtime registration; the compiler generates a
  compile-time factory registry.
- See [`custom-elements-v1.md`](custom-elements-v1.md).

## 3.3 Text input nodes

```json
{
  "kind": "TextInput",
  "id": "node::email",
  "uiId": "555555555555555555",
  "props": {
    "value": { "kind": "ref", "id": "state::email" },
    "placeholder": "Email",
    "inputMode": "email",
    "autofill": "email"
  },
  "events": {
    "onChange": "bind::on_email_changed",
    "onSubmit": "bind::on_submit"
  },
  "children": []
}
```

- `TextInput`/`TextArea` are leaf intrinsics; `children` must be empty.
- Editing state/commands/IME (input method editor)/undo/clipboard are defined by
  [`text-editing-v1.md`](text-editing-v1.md).
- `value`/`defaultValue`/selection are typed; validation is a form concern.
- `secure` fields redact diagnostics/snapshots and disable clipboard by default (see
  [`platform-shell-v1.md`](platform-shell-v1.md) section 2).
- `Scroll` behavior/physics is defined by [`scroll-and-gestures-v1.md`](scroll-and-gestures-v1.md).

## 3.4 Component nodes

```json
{
  "kind": "Component",
  "id": "node::5",
  "uiId": "666666666666666666",
  "component": "comp::Row",
  "props": { "label": { "kind": "ref", "id": "item.label" } },
  "events": { "onPick": "handler::1" },
  "children": []
}
```

- `component` names another component of the project; the node is not replaced by that component's
  root, so each instance keeps its own state, effects and refs.
- `props` holds one entry per prop given in the source, as a literal or an Expression; `events`
  holds callback props. Both are validated against the referenced component's props (section 9).
- A `Component` node may carry `key`, `condition` and `loop` like any other node.
- **Children (static slot).** A component declares `{ "name": "children", "type": "children" }` in
  `props`. Only then may a `Component` node of it have a non-empty `children`; those nodes are the
  slot content. The slot content belongs to the caller: it is part of the caller's tree, takes its
  `node::` ids in the caller's pre-order, and reads the caller's state, props and loop variables,
  never the callee's. A callee places the content with a `Slot` node, a leaf with no props, style or
  events that is replaced by the instance's children at that position:

  ```json
  {
    "kind": "Slot",
    "id": "node::7",
    "uiId": "777777777777777777",
    "source": { "file": "ui/Card.tsx", "offset": 42, "length": 10, "line": 3, "column": 5, "endLine": 3, "endColumn": 15 }
  }
  ```

  A `Component` node without children renders nothing at the `Slot`. The declaration is checked:
  a `Component` node with children whose target does not declare the prop is `TESSERA_IR_SLOT`, and
  a `Slot` in a component without the prop is `TESSERA_IR_SLOT` too.
- A component has at most one `children` prop and at most one `Slot`, and a `Slot` is not inside a
  `loop` template of the callee or inside another component's slot content. A `Slot` in a
  component without a `children` prop is an error. The reference graph is acyclic: a component
  that reaches itself through `Component` nodes is `TESSERA_IR_CYCLE`.
- `Component` and `Slot` are the only additions to the node kinds in v1; both are additive, so
  `irVersion` stays `1` (section 12).

## Continued in part 2

Sections 4-14 live in [`ir-v1-expr.md`](ir-v1-expr.md); numbering is unchanged.

| Section | Topic |
|---|---|
| 4 | Expressions (including the `record` value, section 4.3) |
| 5 | Loop |
| 5.1 | ErrorBoundary |
| 6 | Binding |
| 7 | Stylesheet references |
| 8 | Source spans |
| 9 | Validation (including the record diagnostics and the writer/validator coverage table) |
| 10 | Codegen mapping |
| 11 | Determinism |
| 12 | Versioning |
| 13 | Example: Counter |
| 14 | Open questions |
