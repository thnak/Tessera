# ADR-051: IR record types and record values (object-literal defaults)

- Status: Proposed (2026-11-13), awaiting the owner
- Date: 2026-11-13
- Related: [`ir-v1.md`](../docs/specs/ir-v1.md) (sections 2, 4, 9, 11, 12),
  [`jsx-subset-v1.md`](../docs/specs/jsx-subset-v1.md) (sections 5, 8.5),
  [`jsx-ir-lowering.md`](../docs/design/jsx-ir-lowering.md) (decision 8),
  [`versioning-v1.md`](../docs/specs/versioning-v1.md), GitHub issue #158 (blocked on this ADR)

## Context

A record is a TypeScript object type with named, typed members (`type Size = { w: int; h: int }`).
Today the IR has no record type:

- A record type appears only as a bare identifier in `type` (`"Size"`). The validator accepts any
  identifier "until a type table exists" (`ir-v1.md` section 11, `validate_expr.cpp`), so a typo in
  a record name is not an error and a field access `size.w` is not checked.
- There is no expression for a record value. The lowering rejects an object literal with
  `TESSERA_IR_UNSUPPORTED` ("an object literal is not an IR expression", `expression_lowering.cpp`).
  `jsx-subset-v1.md` section 8.5 already allows an object literal as a `useState` initial of a
  declared record type, but nothing can lower it. The same gap blocks `props = { size: { w: 1 } }`
  defaults (issue #158, the last open #70 leftover).
- Codegen cannot emit a record default: it does not know the members or their order.

## Decision (proposed)

Add a module-level type table and one new expression kind. Both are additive and optional, so
`irVersion` stays `1` (`ir-v1.md` section 12); a module without records is byte-identical to today.

### 1. Type table

`IrModule` gets an optional `types` array, sorted by `name` (determinism, section 11):

```json
"types": [
  { "id": "type::Size", "name": "Size",
    "members": [
      { "name": "w", "type": "int" },
      { "name": "h", "type": "int", "optional": false }
    ],
    "source": { "file": "ui/Card.tsx", "offset": 0, "length": 40, "line": 1, "column": 1,
                "endLine": 1, "endColumn": 41 } }
]
```

- Members keep declaration order (it is the aggregate-initialization order in generated C++).
- A member type is any IR type of section 2: primitives, `list<T>`, `optional<T>`, or another record
  by name. No `callback`, no `children`, no function members. A record may not contain itself,
  directly or through other records (`TESSERA_IR_TYPE_CYCLE`).
- Record names are unique in a module. The framework payload records (`TextChangeEvent`, the typed
  event objects of `jsx-subset-v1.md` section 16.2) are predeclared and not listed in `types`.
- The identity is the name, as for components: two files declaring the same name is
  `TESSERA_IR_DUPLICATE_ID`, not a merge.

### 2. Record expression

```json
{ "kind": "record", "type": "Size",
  "fields": [ { "name": "w", "value": 1 }, { "name": "h", "value": 2 } ] }
```

- `fields` lists every non-optional member exactly once, in member order; an optional member may be
  omitted (its value is empty). Each value is an Expression of the member type.
- Values are pure Expressions (section 4), so a field may be a literal, a `ref` to a prop, a nested
  `record`, or an `array` of the allowed literals. Dynamic values are allowed in a `useState`
  `initial` and a prop `default`, which are evaluated once at mount.
- No spread, no computed keys, no shorthand with a function value (the rule of
  `jsx-subset-v1.md` section 8.5 is unchanged).

### 3. Where a record value is allowed

Only where the subset already allows an object literal for a record: a `useState` initial, a prop
default (new), and a field of another record value. A record value in a binary expression, a
condition or a template is a hard error (`TESSERA_IR_RECORD_VALUE`); only its members take part.

### 4. Validation (section 9 additions)

Hard errors: an unknown record name (replaces the "any identifier" rule; `TESSERA_IR_UNKNOWN_TYPE`),
a `record` whose `type` is not in the table, a missing, duplicate or unknown field, a field value
whose type does not match the member, a dotted `ref` to a member the record does not have, and a
type cycle. Warning: a declared record no component uses.

### 5. Codegen

A record lowers to a C++ aggregate `struct` emitted once per module in declaration order of use
(dependencies first), in the generated header of the first component that uses it. A record
expression becomes designated aggregate initialization (`Size{.w = 1, .h = 2}`), so member order and
names are compiler-checked. Equality and hashing are not generated in this slice (a record is not a
state-dependency comparison key yet; changing a record state value always schedules an update, as
for a list). Records contain no heap allocation beyond their own `string`/`list` members.

## Alternatives considered

- **Flatten a record default into one prop per member.** No IR change, but it breaks the component
  signature the developer wrote and cannot express a record-typed state.
- **A generic `object` expression with untyped keys.** Smaller IR, but it gives up the compile-time
  checks the type table provides and cannot be emitted as an aggregate.
- **Wait for the full type table (slice 4) of `jsx-ir-lowering.md`.** This ADR is that table, scoped
  to the part issue #158 and `useState` records need.

## Consequences

- `ir-v1.md` gains section 2.2 (types), a `record` row in section 4, the new diagnostics in section
  9 and a note in section 12 that this is additive. `docs/specs/versioning-v1.md` records no version
  bump. The JSON schema and the writer/validator coverage table (section 9.1) are extended together.
- Lowering collects declared record types (alias table already resolves cross-file aliases) and
  lowers `ExprKind::Object` when the expected type is a declared record.
- Goldens: one new IR snapshot and one codegen golden for a record default; existing goldens do not
  change.
- Risk: an `irVersion` 1 consumer that does not know `types` or `record` would reject a module that
  uses them. In V1 the compiler and the evaluator ship together, so this is acceptable; if the owner
  prefers, the alternative is `irVersion: 2` with the same shape.

## Slices after acceptance (issue #158)

1. Spec: `ir-v1.md` sections above, schema, diagnostics table.
2. IR model, writer, reader, validator (type table, unknown names, cycles, field checks).
3. Lowering: collect record types, lower object literals for `useState` initials and prop defaults.
4. Codegen: struct emission and designated initialization, golden, `compiler-demo` unchanged.
5. Evaluator parity for the designer (Wasm) with a differential test.

## Questions for the owner

1. Accept the additive route (`irVersion` stays `1`) or prefer `irVersion: 2`?
2. Are dynamic field values (a `ref` to a prop) allowed in a record default, or literals only for
   the first slice? Literals only is smaller and enough for #158; dynamic values can follow.
3. Should the first slice also lower `useState` record initials (same mechanism), or only prop
   defaults as the issue says? The proposal includes both because they share every step.
