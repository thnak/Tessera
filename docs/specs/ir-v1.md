# Tessera IR v1

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
| `stylesheets` | StylesheetRef[] | no | One per imported `.css` (`id`, `path`, `hash` = `fnv1a64:` of the file bytes), sorted by path |

## 2. Component

```json
{
  "id": "comp::Counter",
  "componentId": "1234567890123456789",
  "name": "Counter",
  "source": { "file": "ui/Counter.tsx", "offset": 0, "length": 310, "line": 1, "column": 1, "endLine": 14, "endColumn": 2 },
  "props": [
    { "name": "initial", "type": "int", "default": 0 }
  ],
  "state": [
    { "id": "state::count", "name": "count", "type": "int", "initial": 0 }
  ],
  "effects": [
    { "id": "effect::sync", "deps": ["state::count"], "handler": "bind::on_count_changed", "cleanup": "bind::on_count_changed_cleanup" }
  ],
  "refs": [
    { "id": "ref::label", "name": "label", "target": "node::2" }
  ],
  "animatedValues": [
    { "id": "anim::opacity", "name": "opacity", "type": "float", "initial": 1.0 }
  ],
  "root": {
    "kind": "View",
    "id": "node::1",
    "uiId": "111111111111111111",
    "source": { "file": "ui/Counter.tsx", "offset": 52, "length": 250, "line": 3, "column": 3, "endLine": 13, "endColumn": 9 },
    "children": []
  }
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
| `root` | Node | yes | Exactly one root node |

`props[].default` and `state[].initial` are Expressions (section 4); a literal is the bare JSON value,
any other expression its object (`{ "kind": "ref", "id": "prop::start" }`). A `Prop` is `{ "name",
"type", "optional"?, "default"? }`: `optional` is the source `?` (the caller may omit the prop), not a
nullable `optional<T>` value; `children` has type `children` and neither. Reference ids are `state::`,
`prop::`, `ref::`, `anim::`, `env::<name>`; a field of a record-typed value is dotted (`item.id`); loop
item/index names are as written. V1 types: `bool`, `int`, `float`, `string`, `color`, `length`,
`node_ref`, `list<T>`, `children` (section 3.4; props only), `optional<T>`, and for props `callback` or a
payload record name (`TextChangeEvent`). No arbitrary objects/functions in state.

## 2.1 Handlers

An inline setter handler ([`jsx-subset-v1.md`](jsx-subset-v1.md) section 9.2) has no protected region, so
it is an IR record of its own, referenced from `events`:

```json
{
  "id": "handler::1",
  "params": [{ "name": "e", "type": "string" }],
  "actions": [
    { "kind": "set", "target": "state::count", "value": { "kind": "binary", "op": "+", "left": { "kind": "ref", "id": "state::count" }, "right": { "kind": "literal", "value": 1 } } },
    { "kind": "animSet", "target": "anim::opacity", "value": { "kind": "literal", "value": 0.5 }, "animate": false }
  ],
  "source": { "file": "ui/Counter.tsx", "offset": 200, "length": 40, "line": 9, "column": 5, "endLine": 9, "endColumn": 45 }
}
```

- `id` is `handler::<N>`, numbered from 1 in source order within the component, and unique in it.
- `actions` run in order. `set` writes a state variable through its setter; `animSet` is an
  animated-value `.set(...)`, with `animate` defaulting to `true`. Values are pure Expressions that may
  read `params`.
- An `animSet` may carry literal timing, omitted when absent (default 200 ms `ease-out`): `durationMs`
  (0..60000), `easing` (a CSS keyword or `cubic-bezier(x1, y1, x2, y2)` with x in [0, 1]) or, exclusive with both and with `animate: false`, `spring` `{stiffness, damping, mass}` in (0, 1e5], [0, 1e4], (0, 1000]; invalid timing is `TESSERA_IR_ANIMATION`.
  `onComplete` (additive) is the `handler::N` of a parameterless handler of the component, run once when the value reaches that set's target (a snap completes at once); unknown: `TESSERA_IR_UNRESOLVED`, with parameters or on a `set`: `TESSERA_IR_ANIMATION`.
- A handler declared once and used by several event props is one record with several references.

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
| `text` | Expr/null | no | Text content; only a `Text` node has this field |
| `children` | Node[] | no | Paint order |
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
- A `VirtualList`/`VirtualGrid` has an `itemTemplate`, no `children` and no `loop` of its own, and
  its template is static (no `condition`, no dynamic component type) and counts toward the section
  5 loop depth; any other kind with `itemTemplate` is `TESSERA_IR_MALFORMED`. A
  missing/multiple/non-`.map` child is `TESSERA_IR_VIRTUAL`; a missing item key is
  `TESSERA_IR_LIST_KEY`; a non-`ref` `.map` source is `TESSERA_IR_UNSUPPORTED` (section 5 loop).
- The field is additive and optional, so `irVersion` stays `1` (section 12). The runtime contract
  is [`virtualization-v1.md`](virtualization-v1.md).

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

## 4. Expressions

V1 expressions are pure and statically analyzable:

| Kind | Shape | Notes |
|---|---|---|
| `literal` | `{ "kind": "literal", "value": 42 }` | bool/int/float/string/null |
| `ref` | `{ "kind": "ref", "id": "state::count" }` | prop/state/ref/animated/environment/local |
| `binary` | `{ "kind": "binary", "op": "+", "left": Expr, "right": Expr }` | `+ - * / % == != < <= > >= && ||` |
| `unary` | `{ "kind": "unary", "op": "!", "value": Expr }` | `! - +` |
| `conditional` | `{ "kind": "conditional", "test": Expr, "then": Expr, "else": Expr }` | no side effects |
| `template` | `{ "kind": "template", "parts": [string|Expr] }` | string interpolation |
| `call` | `{ "kind": "call", "callee": "formatNumber", "args": [Expr] }` | allowlisted pure functions only (section 4.2) |
| `array` | `{ "kind": "array", "items": [Expr] }` | list literal; a value position only (state/prop initial). Lowering accepts literal elements only, per jsx-subset-v1 section 8.4 |

Rejected: member access on arbitrary objects, array indexing except list items, assignments,
function expressions, `await`, `yield`, `new`, dynamic imports, and any call with side effects.

### 4.1 Numeric semantics

These rules apply identically in the IR evaluator (Wasm designer) and in generated C++; a differential test checks this ([`designer.md`](../design/designer.md) section 9). They were decided by the project owner.

| Topic | Rule |
|---|---|
| `int` | Signed 32-bit two's complement |
| `float` | IEEE-754 binary64 (C++ `double`), round to nearest even |
| Integer overflow in `+ - *` and unary `-` | Wraps modulo 2^32 (generated C++ computes in `uint32_t` and converts to `int32_t`, so there is no undefined behavior). `-` of the minimum `int` gives the minimum `int` |
| `/` | Operands convert to `float`; the result is `float`. Division of `float` by zero follows IEEE-754 (`Infinity`, `-Infinity`, `NaN`) |
| `%` on two `int` | Result has the sign of the left operand (as in C++ and JavaScript). `x % 0` gives `0` and reports the runtime diagnostic `TESSERA_IR_INT_MODULO_ZERO` once per expression node per session; the minimum `int` `% -1` gives `0`. `%` with a `float` operand is `fmod`, with IEEE-754 results for zero divisors |
| Mixed `int` and `float` operands | The `int` converts to `float` exactly |
| Comparison and `==`/`!=` on `float` | IEEE-754: any comparison with `NaN` is false except `!=`, which is true |
| `float` to `int` | Never implicit. The function `toInt` truncates toward zero; `NaN` gives `0`; values outside the `int` range saturate to the minimum or maximum `int` |
| Number to string (template, text child) | `int`: base 10 with a leading `-` for negatives. `float`: the shortest decimal text that parses back to the same value (the output of C++ `std::to_chars` without a precision), `NaN`, `Infinity` or `-Infinity`; negative zero prints as `-0`. No locale is ever consulted |

### 4.2 Pure functions

A `call` expression may name only the functions in this table. They are pure, total (never throw), locale-independent, and are named without a namespace. In source they are imported by name from `"tessera"` ([`jsx-subset-v1.md`](jsx-subset-v1.md) section 4). `Math.*` of JavaScript is not available.

| Function | Signature | Result |
|---|---|---|
| `min(a, b)`, `max(a, b)` | `(int, int) -> int`; otherwise `(float, float) -> float` | Smaller / larger value. With `NaN`, the result is the other operand; both `NaN` gives `NaN` |
| `clamp(x, lo, hi)` | same typing as `min` | `min(max(x, lo), hi)` |
| `abs(x)` | `int -> int`, `float -> float` | Absolute value; the minimum `int` stays the minimum `int` |
| `floor(x)`, `ceil(x)`, `round(x)` | `float -> float` | Round toward minus infinity, plus infinity, or to nearest with ties away from zero |
| `toInt(x)` | `float -> int` | As in section 4.1 |
| `toFloat(x)` | `int -> float` | Exact conversion |
| `length(x)` | `string -> int`, `list<T> -> int` | For a string, the number of Unicode code points; for a list, its element count |
| `formatNumber(x, decimals)` | `(int or float, int) -> string` | Fixed-point text with `decimals` digits after a `.` separator (`decimals` is clamped to 0..20), correctly rounded from the exact value of `x` with ties away from zero; no thousands separator; `NaN`, `Infinity`, `-Infinity` as in section 4.1 |

Adding a function to this table is a change to this specification and to the evaluator and the code generator together. Functions that depend on locale, time, randomness or the environment are never allowed.

## 5. Loop

```json
{
  "kind": "loop",
  "item": "item",
  "index": "index",
  "source": { "kind": "ref", "id": "state::rows" }
}
```

The `loop` is a field of the node it repeats; the key is that node's `key`:

```json
{ "kind": "View", "id": "node::10", "key": { "kind": "ref", "id": "item.id" }, "loop": { "kind": "loop", "item": "item", "index": "index", "source": { "kind": "ref", "id": "state::rows" } }, "children": [] }
```

- `source` must be a list ref or a static list literal.
- The node's `key` is required for dynamic lists; a missing key is a hard error.
- The node that carries `loop` and its subtree are the template; `item`/`index` are local refs.
  They are distinct valid local names (no dots), and the source is an existing list reference.
- No nested dynamic loops beyond depth 2 in V1.

## 5.1 ErrorBoundary

```json
{
  "id": "node::err",
  "kind": "ErrorBoundary",
  "props": {
    "resetKey": { "kind": "ref", "id": "state::route" },
    "maxAutoRetries": 0,
    "catchEvents": true
  },
  "events": { "onError": "bind::on_error" },
  "fallback": { "kind": "View", "id": "node::fallback", "children": [] },
  "children": [
    { "kind": "View", "id": "node::child", "children": [] }
  ]
}
```

- `fallback` is required (one node subtree).
- `children` is exactly one subtree in V1; wrap siblings in a `View`.
- `onError` is optional and must resolve to a binding.
- `resetKey` is optional; any value change resets the boundary.
- Nested boundaries are allowed; nearest catches first.
- The boundary is not painted and does not affect layout.
- Semantics/exception policy: [`error-boundaries-v1.md`](error-boundaries-v1.md).

## 6. Binding

```json
{
  "id": "bind::on_click",
  "component": "comp::Counter",
  "kind": "event",
  "source": { "file": "ui/Counter.tsx", "offset": 240, "length": 15, "line": 12, "column": 5, "endLine": 12, "endColumn": 20 },
  "region": "on_click"
}
```

| Field | Type | Notes |
|---|---|---|
| `id` | string | `bind::<id>`, globally unique |
| `component` | string | Owning component id |
| `kind` | string | `state`, `effect`, `ref`, `animated`, `event`, `logic` |
| `source` | SourceSpan | Annotation location |
| `region` | string | Protected-region id (without `bind::`); a protected region is the developer-owned C++ block, see [ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md) |

A binding for a native handler may also carry its signature and preview body
([`jsx-subset-v1.md`](jsx-subset-v1.md) section 9.3), all optional:

| Field | Type | Notes |
|---|---|---|
| `params` | `{ name, type }[]` | Typed parameters, in order; the C++ stub uses them |
| `returnType` | string | IR type; absent means `void` |
| `preview` | `{ actions, return }` | `actions` as in section 2.1; `return` is an optional Expression for a non-`void` handler. Absent when the handler has no preview body |

`kind` must match the use: an event binding is named by a node `events` entry, and an effect (and
its cleanup) is named by `effects[].handler`/`cleanup`. Binding ids are stable across regeneration.
Renames go through a migration; the merger never guesses. See
[`developer-workflow.md`](../guides/developer-workflow.md).

## 7. Stylesheet references

```json
{ "id": "css::app", "path": "ui/app.css", "hash": "sha256:..." }
```

The IR does not embed raw CSS. The build-time style engine compiles CSS to a rule table keyed
by stable rule ids; generated C++ references those ids/classes. Runtime uses the same computed
style semantics. See [`stylesheet-v1.md`](stylesheet-v1.md).

## 8. Source spans

```json
{ "file": "ui/Counter.tsx", "offset": 240, "length": 15, "line": 12, "column": 5, "endLine": 12, "endColumn": 20 }
```

| Field | Type | Meaning |
|---|---|---|
| `file` | string | Project-relative path, POSIX separators |
| `offset` | integer | Byte offset of the first byte of the span in the UTF-8 file, 0-based |
| `length` | integer | Length of the span in bytes |
| `line`, `column` | integer | Start position, 1-based |
| `endLine`, `endColumn` | integer | End position (exclusive), 1-based |

- A column counts Unicode code points from the start of the line (not bytes, not UTF-16 units).
- A leading UTF-8 byte order mark counts in `offset` (offsets are file byte offsets) and is not a
  column: the first character after it is column 1.
- `offset` and `length` are required so that tools (the designer patch writer, [`designer-protocol-v1.md`](designer-protocol-v1.md) section 3.1) can edit text exactly. The examples of sections 2, 3 and 13 show full spans. Other examples in this document are fragments that omit `source` fields; the emitted IR contains them on every component, node, binding and effect.
- Source spans are required on components, nodes, bindings, effects and diagnostics. A node's span covers its whole JSX element (opening tag through closing tag). A component's span covers its whole function declaration.
- Generated C++ may carry `#line`-style source maps after V1 (open question, section 14); V1
  emits a sidecar map.

## 9. Validation

Hard errors:

- `irVersion` missing/unsupported
- duplicate component/node/binding ids
- unresolved `ref`/`binding`/node targets
- unsupported node kind, type, operator, or callee
- `ErrorBoundary` missing `fallback`, more/less than one child subtree, or unresolved `onError`
  binding
- missing/duplicate `componentId`/`uiId`, `automationId != uiId`, or a 64-bit ID collision
- unknown/non-literal `element`, unknown element prop/event, missing shader, or `CustomElement`
  with children
- `TextInput`/`TextArea` with children or unsupported props/event payloads
- dynamic component types, context, suspense, custom hooks
- missing list key or non-static loop source; a loop `item`/`index` that is empty, dotted or equal
  (`TESSERA_IR_LOOP`); a virtual collection without an `itemTemplate`, an item template without its
  `loop`, or a condition on it (`TESSERA_IR_VIRTUAL`)
- a field outside its node kind (`text` on a non-`Text` node, `style` on a `Component` node), a
  template or non-bool literal `condition` (`TESSERA_IR_CONDITION`), or an `ErrorBoundary` fallback
  with `condition`/`loop`
- a binding whose `kind` does not match its use (`TESSERA_IR_BINDING`)
- binding annotation without a matching IR use, or vice versa (a binding no event or effect uses
  is this error, not a warning)
- unresolved or duplicate `handler::` id, an `events` value that is neither a `bind::` nor a
  `handler::` id, a `Component` node with an unresolved `component`, an unknown prop, or a prop or
  event the referenced component does not declare
- a `Component` node with `children` whose component has no `children` prop; a `Slot` outside a
  component with a `children` prop, a second `Slot`, a `Slot` with props, style, events, key,
  condition, loop or children, a `Slot` as the component root, or a `Slot` inside a `loop` template
  or inside another component's slot content
- a component reference cycle (a component whose tree reaches itself), `TESSERA_IR_CYCLE`
- non-deterministic ordering/ids

Warnings:

- unused ref/state
- binding present in existing code but not in generated IR (rename/removal)
- deprecated fields

### 9.1 Writer and validator coverage (slices 6 to 9)

Every construct lowering emits is written; structural invariants are validated. Goldens
`tests/golden/ir/{counter,rich}.json` are re-baselined deliberately with `ir_json_tests --write`;
a full IR reader is still deferred (jsx-ir-lowering decision 15), so round-trip tests compare
canonical text, not a re-parsed model.

| Construct | Writer | Validator |
|---|---|---|
| `condition` | expression object | boolean-valued shape (`TESSERA_IR_CONDITION`) |
| loop `item`/`index`/`source`/`key` | `loop` object; `index` when present | distinct local names, source ref, key required, depth 2 |
| `Component` reference | `component`, props, events, slot `children` | target resolves, no `style`, children only when the target declares the `children` prop, no reference cycle (`TESSERA_IR_CYCLE`) |
| `Slot` | `kind` only (a bare position marker) | at most one per component, only with the `children` prop, no payload, not the root, not in a loop template or slot content (`TESSERA_IR_SLOT`) |
| `VirtualList`/`VirtualGrid` `itemTemplate` | the template node with its `loop` and `key` | one static template, its loop source is a list ref, its key is required, no `children`/own `loop`, no condition (`TESSERA_IR_VIRTUAL`, `TESSERA_IR_LIST_KEY`) |
| text, style/className, events, `fallback` | `text`/`style`/`props`/`events`, fallback node | `text` only on `Text`; event ids resolve to event bindings; fallback static |

Deliberately not covered: intrinsic prop/event schemas (the IR is a leaf and does not link
`tessera_schema`), the `Children` type of a prop other than the `children` declaration (the type
table that resolves prop types is a later slice), and a re-parsed reader (section 9.1 note above).

## 10. Codegen mapping

| IR | Generated C++ |
|---|---|
| Component | struct/class with state/effect/ref/animated members |
| `Component` reference | a generated host node (element kind `Component`) plus one member of the referenced component's struct, built by the caller; the instance owns its own state and lifecycle (`mount`/`update`/`unmount` are driven by the caller) |
| `Slot` | the callee's `build(parent, slot)` overload attaches the caller's slot nodes at the Slot position; the caller builds that content with its own state/props/handlers |
| Node | `std::make_unique<Node>(id)` + setters/add_child |
| Text | text node + style + text layout binding |
| Event binding | `on_event` dispatch to protected region |
| ErrorBoundary | boundary state + guarded child build/update/effect/event + fallback subtree |
| Conditional | `if` around node creation/update |
| Loop | keyed list update/rebuild |
| `VirtualList`/`VirtualGrid` | a fixed container node with generated geometry constants (`itemSize`/`estimateSize`/`overscan`/`gap`, grid row height/columns), a bounded realized-window item table, `apply_virtual_<n>(first, last)` (keyed reconciliation and changed-item repaint), and the generated `scroll_virtual_<n>(scroll_offset, viewport_extent)` entry point, which owns the runtime geometry, refines realized-item measures, re-anchors the offset and dispatches `onScroll`/`onEndReached`; the host may still compute `[first, last)` itself and call `apply_virtual_<n>` with its own geometry |
| Ref | `Node*` member + assignment after creation |
| Animated value | `AnimationController`/`Tween` member |
| Style token | property id/value or stylesheet class reference |

## 11. Determinism

- Components sorted by source path, then name.
- Nodes in source order; bindings sorted by id.
- No timestamps, absolute paths, random ids, or hash-map iteration.
- JSON snapshot output is stable (sorted keys in tests).
- The writer omits empty optional fields (examples that show `[]` or `{}` are only explicit);
  `components`, `bindings` and `stylesheets` are always written.
- V1 limit: the validator accepts any identifier as a record type until a type table exists.
- Repeated compilation of the same input is byte-identical.

## 12. Versioning

- `irVersion: 1` is the only accepted version in V1.
- Adding optional fields or node kinds keeps v1 (precedent: `Component`, `Slot`); an unknown kind is
  `TESSERA_IR_MALFORMED`. Changing meaning or removing fields bumps the version, with a migration.
- Codegen rejects unsupported versions with `TESSERA_IR_VERSION`.
- Versions of all schemas are tabulated in [`versioning-v1.md`](versioning-v1.md).
- Snapshots and `bindings.json` record the IR version.

## 13. Example: Counter

The worked Counter example (a complete `ir.json`) lives in
[`ir-v1-example.md`](ir-v1-example.md) to keep this normative spec under the size limit.

## 14. Open questions

- Virtual collection data source/paging and nested virtualization; see
  [`virtualization-v1.md`](virtualization-v1.md).
- Source-map format and `#line` emission.
- Whether styles are inlined as tokens or referenced by compiled rule ids.
- Expression extensions beyond the functions of section 4.2 (string methods such as
  `toUpperCase`, `trim`, `startsWith`); locale-aware formatting.
- Incremental IR diffs for hot reload.
