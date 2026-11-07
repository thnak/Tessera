# Tessera IR v1

Status: proposed (Big Step C). Normative for `src/compiler/ir/` and codegen.

The IR is the only input to C++ codegen. It is neutral, versioned, serializable to JSON, and
independent of the JSX parser (ADR-007). Codegen must not read the parser AST.

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
  "name": "Counter",
  "source": { "file": "ui/Counter.tsx", "line": 1, "column": 1 },
  "props": [
    { "name": "initial", "type": "int", "default": 0 }
  ],
  "state": [
    { "id": "state::count", "name": "count", "type": "int", "initial": 0 }
  ],
  "effects": [
    { "id": "effect::sync", "deps": ["state::count"], "handler": "bind::on_count_changed" }
  ],
  "refs": [
    { "id": "ref::label", "name": "label", "target": "node::2" }
  ],
  "animatedValues": [
    { "id": "anim::opacity", "name": "opacity", "type": "float", "initial": 1.0 }
  ],
  "root": { "kind": "View", "id": "node::1", "children": [] }
}
```

| Field | Type | Required | Notes |
|---|---|---|---|
| `id` | string | yes | Unique, stable within the project |
| `name` | string | yes | PascalCase component name |
| `source` | SourceSpan | yes | For diagnostics/source maps |
| `props` | Prop[] | no | Typed props |
| `state` | State[] | no | `useState` |
| `effects` | Effect[] | no | `useEffect` |
| `refs` | Ref[] | no | `useRef` |
| `animatedValues` | AnimatedValue[] | no | `useAnimatedValue` |
| `root` | Node | yes | Exactly one root node |

V1 types: `bool`, `int`, `float`, `string`, `color`, `length`, `node_ref`, `list<T>`. No
arbitrary objects/functions in state. Nullable types use `optional<T>`.

## 3. Node

```json
{
  "id": "node::2",
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
| `id` | string | yes | Unique within the component |
| `kind` | string | yes | `View`, `Text`, `Button`, `Image`, `Path`, `Scroll`, `Spacer` |
| `key` | string/int/null | no | Stable list identity |
| `props` | object | no | Typed intrinsic props |
| `style` | object | no | Style tokens; inline style |
| `events` | object | no | Event name -> binding id |
| `text` | Expr/null | no | Text content for `Text` |
| `children` | Node[] | no | Paint order |
| `condition` | Expr/null | no | Static conditional; V1 bool expression |
| `loop` | Loop/null | no | Static list loop |

Node kinds are closed in V1. A custom component reference is lowered to its root node; there
is no dynamic component type.

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
| `call` | `{ "kind": "call", "callee": "formatNumber", "args": [Expr] }` | allowlisted pure functions only |

Rejected: member access on arbitrary objects, array indexing except list items, assignments,
function expressions, `await`, `yield`, `new`, dynamic imports, and any call with side effects.

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
- No nested dynamic loops beyond a documented depth (V1: 2).

## 6. Binding

```json
{
  "id": "bind::on_click",
  "component": "comp::Counter",
  "kind": "event",
  "source": { "file": "ui/Counter.tsx", "line": 12, "column": 5 },
  "region": "on_click"
}
```

| Field | Type | Notes |
|---|---|---|
| `id` | string | `bind::<id>`, globally unique |
| `component` | string | Owning component id |
| `kind` | string | `state`, `effect`, `ref`, `animated`, `event`, `logic` |
| `source` | SourceSpan | Annotation location |
| `region` | string | Protected-region id (without `bind::`) |

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
{ "file": "ui/Counter.tsx", "line": 12, "column": 5, "endLine": 12, "endColumn": 20 }
```

- 1-based line/column.
- Paths are project-relative, POSIX separators.
- Source spans are required on components, nodes, bindings, and diagnostics.
- Generated C++ may carry `#line`-style source maps later; V1 emits a sidecar map.

## 9. Validation

Hard errors:

- `irVersion` missing/unsupported
- duplicate component/node/binding ids
- unresolved `ref`/`binding`/node targets
- unsupported node kind, type, operator, or callee
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
- Snapshots and `bindings.json` record the IR version.

## 13. Example: Counter

```json
{
  "irVersion": 1,
  "module": { "name": "demo", "imports": ["tessera"] },
  "components": [
    {
      "id": "comp::Counter",
      "name": "Counter",
      "source": { "file": "ui/Counter.tsx", "line": 1, "column": 1 },
      "props": [],
      "state": [
        { "id": "state::count", "name": "count", "type": "int", "initial": 0 }
      ],
      "effects": [],
      "refs": [],
      "animatedValues": [],
      "root": {
        "id": "node::1",
        "kind": "View",
        "props": { "className": "counter" },
        "style": { "display": "flex", "gap": "8px" },
        "children": [
          {
            "id": "node::2",
            "kind": "Text",
            "text": {
              "kind": "template",
              "parts": ["Count: ", { "kind": "ref", "id": "state::count" }]
            }
          },
          {
            "id": "node::3",
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
      "source": { "file": "ui/Counter.tsx", "line": 12, "column": 5 },
      "region": "on_click"
    }
  ],
  "stylesheets": [
    { "id": "css::app", "path": "ui/app.css", "hash": "sha256:placeholder" }
  ]
}
```

## 14. Open questions

- Static list model and virtualization interaction.
- Source-map format and `#line` emission.
- Whether styles are inlined as tokens or referenced by compiled rule ids.
- Expression extensions (string methods, formatting) and their purity allowlist.
- Incremental IR diffs for hot reload.
