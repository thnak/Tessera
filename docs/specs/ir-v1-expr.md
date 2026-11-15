# Tessera IR v1: expressions, validation and versioning

Status: proposed. Second part of the normative [Tessera IR v1](ir-v1.md) (split to respect the
600-line document limit, the same way [`jsx-subset-v1.md`](jsx-subset-v1.md) is split). Sections
1-3 (top level, the component model, handlers, node kinds and the record type table) are in part 1;
section numbers continue from there, so every `section N` reference in the specs and ADRs stays
valid.

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
| `record` | `{ "kind": "record", "type": "Size", "fields": [{ "name": "w", "value": Expr }] }` | record value (section 4.3, ADR-051); a value position only |

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

### 4.3 Record values (additive, ADR-051)

```json
{ "kind": "record", "type": "Size",
  "fields": [ { "name": "w", "value": { "kind": "literal", "value": 1 } },
              { "name": "h", "value": { "kind": "literal", "value": 2 } } ] }
```

- `type` is a record declared in `types` (section 2.2). A payload record is framework data and has no
  record value (`TESSERA_IR_UNSUPPORTED`); an undeclared name is `TESSERA_IR_UNKNOWN_TYPE`.
- `fields` lists **every non-optional member exactly once, in member order**; an optional member may
  be omitted (its value is empty in the generated aggregate). A missing, duplicate or unknown field
  is `TESSERA_IR_RECORD_VALUE`; so is a field value whose type is not the member's, and fields
  written out of member order are `TESSERA_IR_ORDER`.
- Each `value` is an Expression of the member type. **First slice:** a field value is a literal, a
  nested `record` of literals, or an `array` of literals of the member element type. A `ref` or any
  other dynamic Expression in a field is `TESSERA_IR_UNSUPPORTED` until a later slice; the field is
  still an Expression in the format, so allowing dynamic values later is not a format change.
- No spread, no computed keys, no shorthand with a function value (the rule of
  [`jsx-subset-v1.md`](jsx-subset-v1.md) section 8.5 is unchanged).
- **Where a record value is allowed.** Only a `useState` initial of a record type, a prop default of
  a record type, or a field of another record value. A record value anywhere else (a binary operand,
  a condition, a template part, an event payload) is `TESSERA_IR_RECORD_VALUE` and only its members
  take part.
- **Dotted members.** A dotted `ref` into a declared record must name a member the record has
  (`TESSERA_IR_UNRESOLVED` otherwise), through any `optional<T>` wrapper; a `list<T>` member is not
  indexable in V1 (`TESSERA_IR_UNRESOLVED`). A dotted ref into a payload record is not member-checked
  (section 9.1).
- **Additive.** The expression kind is optional and `irVersion` stays `1` (section 12); a module
  without a record value is byte-identical to the IR before it existed.

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
| `region` | string | Binding id (without `bind::`); the name of the required developer-class member, see [ADR-040](../../decisions/ADR-040-generated-base-and-developer-class.md) |

A binding for a native handler may also carry its signature and preview body
([`jsx-subset-v1.md`](jsx-subset-v1.md) section 9.3), all optional:

| Field | Type | Notes |
|---|---|---|
| `params` | `{ name, type }[]` | Typed parameters, in order; the C++ stub uses them |
| `returnType` | string | IR type; absent means `void` |
| `preview` | `{ actions, return }` | `actions` as in section 2.1; `return` is an optional Expression for a non-`void` handler. Absent when the handler has no preview body |

`kind` must match the use: an event binding is named by a node `events` entry, and an effect (and
its cleanup) is named by `effects[].handler`/`cleanup`. Binding ids are stable across regeneration.
Renames go through a reviewed edit of the marker and the member; there is no merge step and no
migration tool. See
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
- Source spans are required on components, nodes, bindings, effects and diagnostics. A node's span covers its whole JSX element (opening tag through closing tag). A component's span covers its whole function declaration. A record type's span covers its declaration (section 2.2).
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
- an inline span violation: a non-span `Text` child, a non-literal span `text`, a nested span, a
  disallowed span field, or a span count/byte cap (`TESSERA_IR_SPAN`, section 3.6)
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
- **records (section 2.2, ADR-051):**
  - a type name that is not a primitive, `callback`, `children`, a declared record or a payload
    record (`TESSERA_IR_UNKNOWN_TYPE`) — this replaces the old "any identifier is a record type"
    rule of section 11
  - a `record` value whose `type` is not in the table (`TESSERA_IR_UNKNOWN_TYPE`), or is a payload
    record (`TESSERA_IR_UNSUPPORTED`)
  - a missing, duplicate or unknown field, a field value whose type is not the member's, or a
    record value in a position other than a `useState` initial, a prop default or a field of another
    record value (`TESSERA_IR_RECORD_VALUE`)
  - a record that contains itself, directly or through other records (`TESSERA_IR_TYPE_CYCLE`)
  - a record member of type `callback` or `children` (`TESSERA_IR_UNSUPPORTED`)
  - a dotted `ref` to a member the record does not have (`TESSERA_IR_UNRESOLVED`)
  - two entries with the same record name (`TESSERA_IR_DUPLICATE_ID`)
- non-deterministic ordering/ids

Warnings: unused ref/state; a binding present in existing code but not in generated IR
(rename/removal); deprecated fields; **a declared record no component uses** (`TESSERA_IR_UNUSED_TYPE`,
section 2.2).

### 9.1 Writer and validator coverage (slices 6 to 9)

Every construct lowering emits is written; structural invariants are validated. Goldens
`tests/golden/ir/{counter,rich}.json` are re-baselined deliberately with `ir_json_tests --write`; a
full IR reader is still deferred (jsx-ir-lowering decision 15), so round-trip tests compare canonical
text, not a re-parsed model.

| Construct | Writer | Validator |
|---|---|---|
| `condition` | expression object | boolean-valued shape (`TESSERA_IR_CONDITION`) |
| loop `item`/`index`/`source`/`key` | `loop` object; `index` when present | distinct local names, source ref, key required, depth 2 |
| `Component` reference | `component`, props, events, slot `children` | target resolves, no `style`, children only when the target declares the `children` prop, no reference cycle (`TESSERA_IR_CYCLE`) |
| `Slot` | `kind` only (a bare position marker) | at most one per component, only with the `children` prop, no payload, not the root, not in a loop template or slot content (`TESSERA_IR_SLOT`) |
| `VirtualList`/`VirtualGrid` `itemTemplate` | the template node with its `loop` and `key` | one static template, its loop source is a list ref, its key is required, no `children`/own `loop`, no condition (`TESSERA_IR_VIRTUAL`, `TESSERA_IR_LIST_KEY`) |
| text, style/className, events, `fallback` | `text`/`style`/`props`/`events`, fallback node | `text` only on `Text`; event ids resolve to event bindings; fallback static |
| inline spans | `Text` children with literal `text`; parent has no `text` | children are all spans, literal text, no nested span or disallowed field, count/byte caps (`TESSERA_IR_SPAN`) |
| `types` (record table) | the `types` array, sorted by `name`, omitted when empty; `members` with `optional` only when true | id is `type::<name>`, unique names and members, sorted order, span, a value member type (`TESSERA_IR_UNSUPPORTED` for `callback`/`children`), every named record declared (`TESSERA_IR_UNKNOWN_TYPE`), no cycle (`TESSERA_IR_TYPE_CYCLE`), unused warning (`TESSERA_IR_UNUSED_TYPE`) |
| `record` value | `kind`, `type`, `fields` in member order, each value an expression object | declared record type, every non-optional member once in member order, no duplicate/unknown field, member-typed field values, three allowed positions, a literal/nested-record/array-of-literals value (`TESSERA_IR_RECORD_VALUE`, `TESSERA_IR_UNSUPPORTED`, `TESSERA_IR_UNKNOWN_TYPE`) |
| dotted `ref` member | the `id` string with its `.member` path | the base resolves, and every segment is a member of the declared record (`TESSERA_IR_UNRESOLVED`) |

Deliberately not covered: intrinsic prop/event schemas (the IR is a leaf and does not link
`tessera_schema`), the `Children` type of a prop other than the `children` declaration, a dotted
`ref` into a framework payload record (its member table is element-schema data: `TextChangeEvent` and
friends are declared by `tessera_schema`, not by the module), and a re-parsed reader (section 9.1
note above).

## 10. Codegen mapping

| IR | Generated C++ |
|---|---|
| Component | struct/class with state/effect/ref/animated members |
| `Component` reference | a generated host node (element kind `Component`) plus one member of the referenced component's struct, built by the caller; the instance owns its own state and lifecycle (`mount`/`update`/`unmount` are driven by the caller) |
| `Slot` | the callee's `build(parent, slot)` overload attaches the caller's slot nodes at the Slot position; the caller builds that content with its own state/props/handlers |
| Node | `std::make_unique<Node>(id)` + setters/add_child |
| Text | text node + style + text layout binding |
| Inline spans | the parent `Text` node gets the concatenated paint text plus one span entry per child (span node id and byte range); each span is emitted as a child scene node with its class/style/role/events so the runtime resolves its run style through the normal cascade |
| Event binding | `on_event` dispatch to the developer-class member named by the binding id |
| ErrorBoundary | boundary state + guarded child build/update/effect/event + fallback subtree |
| Conditional | `if` around node creation/update |
| Loop | keyed list update/rebuild |
| `VirtualList`/`VirtualGrid` | a fixed container node with generated geometry constants (`itemSize`/`estimateSize`/`overscan`/`gap`, grid row height/columns), a bounded realized-window item table, `apply_virtual_<n>(first, last)` (keyed reconciliation and changed-item repaint), and the generated `scroll_virtual_<n>(scroll_offset, viewport_extent)` entry point, which owns the runtime geometry, refines realized-item measures, re-anchors the offset and dispatches `onScroll`/`onEndReached`; the host may still compute `[first, last)` itself and call `apply_virtual_<n>` with its own geometry |
| Ref | `Node*` member + assignment after creation |
| Animated value | `AnimationController`/`Tween` member |
| Style token | property id/value or stylesheet class reference |

A record type and a `record` value need codegen support that this slice does not add: the struct is
emitted once per module (dependencies first) and the value becomes designated aggregate
initialization (`Size{.w = 1, .h = 2}`), so member order and names are compiler-checked (ADR-051
decision 5). That is the follow-up codegen slice; until it lands, codegen reports a record type as
having no C++ form rather than emitting something wrong.

## 11. Determinism

- Components sorted by source path, then name; nodes in source order; bindings sorted by id; record
  types sorted by name.
- No timestamps, absolute paths, random ids, or hash-map iteration.
- JSON snapshot output is stable (sorted keys in tests).
- The writer omits empty optional fields (examples that show `[]` or `{}` are only explicit);
  `components`, `bindings` and `stylesheets` are always written, and `types` is written only when a
  record is declared.
- A type name resolves against the module's `types` table and the framework's predeclared payload
  records (section 2.2); any other identifier is `TESSERA_IR_UNKNOWN_TYPE`. Repeated compilation of
  the same input is byte-identical.

## 12. Versioning

- `irVersion: 1` is the only accepted version in V1.
- Adding optional fields or kinds keeps v1 (precedent: `Component`, `Slot`, inline spans, and now the
  optional `types` array and the `record` expression of section 2.2/4.3, ADR-051); an unknown kind is
  `TESSERA_IR_MALFORMED`. Changing meaning or removing fields bumps the version, with a migration.
  Codegen rejects unsupported versions with `TESSERA_IR_VERSION`.
- The record additions are **additive and optional**: a module that declares no record writes exactly
  the bytes it wrote before them (`irVersion` stays `1`, and `tests/golden/ir/{counter,rich}.json` are
  unchanged). An `irVersion` 1 consumer that does not know `types`/`record` would reject a module that
  uses them; in V1 the compiler and the evaluator ship together, so that is the accepted risk
  (ADR-051, owner decision).
- Versions of all schemas are tabulated in [`versioning-v1.md`](versioning-v1.md).
- Snapshots and `bindings.json` record the IR version.

## 13. Example: Counter

The worked Counter example (a complete `ir.json`) lives in [`ir-v1-example.md`](ir-v1-example.md) to
keep this normative spec under the size limit.

## 14. Open questions

- Virtual collection data source/paging and nested virtualization
  ([`virtualization-v1.md`](virtualization-v1.md)); source-map format and `#line` emission; whether
  styles are inlined as tokens or referenced by compiled rule ids.
- Expression extensions beyond the functions of section 4.2 (string methods such as
  `toUpperCase`, `trim`, `startsWith`); locale-aware formatting.
- Incremental IR diffs for hot reload.
- Record values: the dynamic field values the first slice rejects (section 4.3), and a `list<record>`
  member's value form (declarable now, no value form yet). ADR-051 lists lowering, codegen and
  evaluator parity as the follow-up slices.
