# Tessera IR v1

Status: proposed (Big Step C, the compiler vertical slice, defined in
[AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step), section "Big Steps"). Normative for `src/compiler/ir/` and
codegen. "V1" in this file means the first-release scope ([AGENTS.md](../../AGENTS.md#non-negotiables) rule 8, "V1 scope
discipline"); "IR" means the compiler intermediate representation (see the
[glossary](../reference/glossary.md)).

The IR is the only input to C++ codegen. It is neutral, versioned, serializable to JSON, and
independent of the JSX parser
([ADR-007](../../decisions/ADR-007-compiler-ir.md), a neutral IR between JSX and C++ codegen;
the JSX front end is described in
[ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md)). Codegen must not
read the parser AST.

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
| `stylesheets` | StylesheetRef[] | no | Compiled CSS references |

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
| `effects` | Effect[] | no | `useEffect`. An Effect has `id`, `deps`, `handler` (binding id), `source`, and an optional `cleanup` (binding id of the cleanup handler; it runs before the effect runs again and on unmount) |
| `refs` | Ref[] | no | `useRef` |
| `animatedValues` | AnimatedValue[] | no | `useAnimatedValue` |
| `root` | Node | yes | Exactly one root node |

V1 types: `bool`, `int`, `float`, `string`, `color`, `length`, `node_ref`, `list<T>`. No
arbitrary objects/functions in state. Nullable types use `optional<T>`.

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
  "style": { "fontSize": "14px", "color": "var(--text)" },
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
| `kind` | string | yes | `View`, `Text`, `Button`, `TextInput`, `TextArea`, `Image`, `Path`, `Scroll`, `Spacer`, `VirtualList`, `VirtualGrid`, `ErrorBoundary`, `CustomElement` |
| `element` | string | for `CustomElement` | Static element id from `ui/elements.json` |
| `source` | SourceSpan | yes | Span of the node's JSX element (section 8) |
| `key` | string/int/null | no | Stable list identity |
| `props` | object | no | Typed intrinsic props |
| `style` | object | no | Style tokens; inline style |
| `events` | object | no | Event name -> binding id |
| `text` | Expr/null | no | Text content for `Text` |
| `children` | Node[] | no | Paint order |
| `condition` | Expr/null | no | Static conditional; V1 bool expression |
| `loop` | Loop/null | no | Static list loop |

Node kinds are closed in V1. A custom component reference is lowered to its root node; there
is no dynamic component type. `CustomElement` is an intrinsic native-render node, not a dynamic
component type; see [`custom-elements-v1.md`](custom-elements-v1.md).

`VirtualList` and `VirtualGrid` are collection nodes defined in
[`virtualization-v1.md`](virtualization-v1.md). They carry a data-source ref/list, a static item
template, overscan/estimate/gap, a required item key, and optional sticky headers. They do not
represent VM/container virtualization or a virtual DOM.

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

## 4. Expressions

V1 expressions are pure and statically analyzable:

| Kind | Shape | Notes |
|---|---|---|
| `literal` | `{ "kind": "literal", "value": 42 }` | bool/int/float/string/null |
| `ref` | `{ "kind": "ref", "id": "state::count" }` | prop/state/ref/animated/local |
| `binary` | `{ "kind": "binary", "op": "+", "left": Expr, "right": Expr }` | `+ - * / % == != < <= > >= && ||` |
| `unary` | `{ "kind": "unary", "op": "!", "value": Expr }` | `! - +` |
| `conditional` | `{ "kind": "conditional", "test": Expr, "then": Expr, "else": Expr }` | no side effects |
| `template` | `{ "kind": "template", "parts": [string|Expr] }` | string interpolation |
| `call` | `{ "kind": "call", "callee": "formatNumber", "args": [Expr] }` | allowlisted pure functions only (section 4.2) |

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
  "source": { "kind": "ref", "id": "state::rows" },
  "key": { "kind": "ref", "id": "item.id" },
  "body": { "kind": "View", "id": "node::10", "children": [] }
}
```

- `source` must be a list ref or a static list literal.
- `key` is required for dynamic lists; missing keys are a hard error.
- Loop body is a node subtree; `item`/`index` are local refs.
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

Binding ids are stable across regeneration. Renames go through a migration; the merger never
guesses. See [`developer-workflow.md`](../guides/developer-workflow.md).

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
- missing list key or non-static loop source
- binding annotation without a matching IR use, or vice versa
- non-deterministic ordering/ids

Warnings:

- unused binding/ref/state
- binding present in existing code but not in generated IR (rename/removal)
- deprecated fields

## 10. Codegen mapping

| IR | Generated C++ |
|---|---|
| Component | struct/class with state/effect/ref/animated members |
| Node | `std::make_unique<Node>(id)` + setters/add_child |
| Text | text node + style + text layout binding |
| Event binding | `on_event` dispatch to protected region |
| ErrorBoundary | boundary state + guarded child build/update/effect/event + fallback subtree |
| Conditional | `if` around node creation/update |
| Loop | keyed list update/rebuild |
| Ref | `Node*` member + assignment after creation |
| Animated value | `AnimationController`/`Tween` member |
| Style token | property id/value or stylesheet class reference |

## 11. Determinism

- Components sorted by source path, then name.
- Nodes in source order; bindings sorted by id.
- No timestamps, absolute paths, random ids, or hash-map iteration.
- JSON snapshot output is stable (sorted keys in tests).
- Repeated compilation of the same input is byte-identical.

## 12. Versioning

- `irVersion: 1` is the only accepted version in V1.
- Adding optional fields keeps v1; changing meaning/removing fields bumps the version and ships
  a migration.
- Codegen rejects unsupported versions with `TESSERA_IR_VERSION`.
- Versions of all schemas are tabulated in [`versioning-v1.md`](versioning-v1.md).
- Snapshots and `bindings.json` record the IR version.

## 13. Example: Counter

```json
{
  "irVersion": 1,
  "module": { "name": "demo", "imports": ["tessera"] },
  "components": [
    {
      "id": "comp::Counter",
      "componentId": "1234567890123456789",
      "name": "Counter",
      "source": { "file": "ui/Counter.tsx", "offset": 0, "length": 310, "line": 1, "column": 1, "endLine": 14, "endColumn": 2 },
      "props": [],
      "state": [
        { "id": "state::count", "name": "count", "type": "int", "initial": 0 }
      ],
      "effects": [],
      "refs": [],
      "animatedValues": [],
      "root": {
        "id": "node::1",
        "uiId": "111111111111111111",
        "source": { "file": "ui/Counter.tsx", "offset": 52, "length": 250, "line": 3, "column": 3, "endLine": 13, "endColumn": 9 },
        "kind": "View",
        "props": { "className": "counter" },
        "style": { "display": "flex", "gap": "8px" },
        "children": [
          {
            "id": "node::2",
            "uiId": "222222222222222222",
            "source": { "file": "ui/Counter.tsx", "offset": 80, "length": 52, "line": 4, "column": 5, "endLine": 4, "endColumn": 57 },
            "kind": "Text",
            "text": {
              "kind": "template",
              "parts": ["Count: ", { "kind": "ref", "id": "state::count" }]
            }
          },
          {
            "id": "node::3",
            "uiId": "333333333333333333",
            "source": { "file": "ui/Counter.tsx", "offset": 140, "length": 60, "line": 5, "column": 5, "endLine": 5, "endColumn": 65 },
            "kind": "Button",
            "props": { "label": "Increment" },
            "events": { "onClick": "bind::on_click" },
            "children": []
          }
        ]
      }
    }
  ],
  "bindings": [
    {
      "id": "bind::on_click",
      "component": "comp::Counter",
      "kind": "event",
      "source": { "file": "ui/Counter.tsx", "offset": 240, "length": 15, "line": 12, "column": 5, "endLine": 12, "endColumn": 20 },
      "region": "on_click"
    }
  ],
  "stylesheets": [
    { "id": "css::app", "path": "ui/app.css", "hash": "sha256:placeholder" }
  ]
}
```

## 14. Open questions

Each item has no decision yet.

- Virtual collection data source/paging and nested virtualization; see
  [`virtualization-v1.md`](virtualization-v1.md).
- Source-map format and `#line` emission.
- Whether styles are inlined as tokens or referenced by compiled rule ids.
- Expression extensions beyond the functions of section 4.2 (string methods such as
  `toUpperCase`, `trim`, `startsWith`); locale-aware formatting.
- Incremental IR diffs for hot reload.
