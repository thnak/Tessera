# Component parts v1: `::part(name)`

Status: implemented (issue #235, ADR-047 item 3). This is the normative spec of the parts selector;
the rest of the stylesheet language stays in [`stylesheet-v1.md`](stylesheet-v1.md) (section 6 for
selectors, section 7 for the cascade, section 15 for diagnostics). Terms: a **part** is an inner
element of a component that the component exposes for styling; the **host** is the component
instance that owns it; a **part element** is the scene node carrying the `part` attribute.

## 1. The `part` attribute

A component's inner elements declare `part="<name>"` (whitespace-separated names, a static literal
like `className`; the common attribute of
[`jsx-intrinsics-v1/common-and-events.md`](jsx-intrinsics-v1/common-and-events.md) section 1 lowers
it to `Node::part()`). The host's stylesheet then targets a part with `::part()`:

```css
Button::part(label) { color: red; }            /* the `label` part of any `<Button>` */
.card::part(label):hover { color: blue; }      /* host `.card`, the part's own `:hover` */
Field::part(label value) { font-weight: 600; } /* every listed name must be present */
```

A library component publishes the parts it declares; an app component does not have to.

## 2. Two elements, one boundary

- A `::part()` compound describes two nodes. The tag, id, classes, attributes and pseudo-classes
  written *before* `::part()` match the **host**: the nearest ancestor component instance
  (`Node::component_name()`; its type selector is the component name and `element_kind` stays
  `Component`). The `::part()` itself, and any pseudo-classes *after* it, match the **part element**,
  which must carry every listed name (order does not matter).
- The host is the nearest enclosing component instance, so a rule never reaches into a nested
  component (`.card::part(label)` does not style a part of a `Card` nested in another `Card`) and
  `::part()` never matches a node without a component ancestor.
- State pseudo-classes after `::part()` (`:hover`, `:active`, `:focus`, `:focus-visible`,
  `:disabled`, `:checked`, ...) read the part element's state. Structural ones are accepted and
  evaluated against the part element; CSS forbids them there, so do not rely on them.

## 3. Grammar and restrictions

`::part(<ident>+)` is a parse error — never a silent non-match — when any of these holds: it does not
use two colons, the argument is empty or holds anything but unquoted idents, it is not the only
pseudo-element of its compound, it appears anywhere but the subject compound (`.a::part(x) .b`), it
appears twice in one compound, or anything element-shaped follows it.

## 4. Specificity and the cascade

- `::part()` counts as a pseudo-element (one type component) and the part names add nothing, as in
  CSS Shadow Parts: `Button::part(label)` is `(0,0,2)`, `.card::part(label):hover` is `(0,2,1)`,
  `::part(label)` is `(0,0,1)`.
- The declarations land on the part element across the boundary, so a host-side `::part()` rule and a
  rule inside the component are one cascade: origin, importance, layer, specificity and source order,
  exactly as in [`stylesheet-v1.md`](stylesheet-v1.md) section 7. Unlayered app CSS still beats the
  library's `@layer tessera-ui` (section 7.2 there).
- Setting or clearing `part` marks the node `StyleDirty`; changing a component name marks its
  subtree, because every `::part()` rule below it re-resolves.

## 5. `@supports selector()`

`@supports selector(<selector list>)` answers whether the engine can match with a selector: the list
parses and every pseudo-class and pseudo-element it uses is implemented. `::part()` is implemented, so
`@supports selector(.card::part(label))` is true; `:has()` and the parse-only pseudo-elements
(`::before`, `::after`, `::placeholder`, `::selection`) answer false because they never match a scene
node. A stylesheet can gate its part rules that way:

```css
@supports selector(::part(label)) {
  .card::part(label) { color: red; }
}
```

`SelectorIndex` buckets a `::part()` subject by its first part name, so matching stays allocation-free:
the host pointer and the part names are captured when the style tree is built.

## 6. Diagnostics

A part name a library component does not declare is a `TESSERA_CSS_UNDECLARED_PART` diagnostic
([`stylesheet-v1.md`](stylesheet-v1.md) section 15) emitted by the codegen CLI. App components are
never diagnosed, because only a library component publishes its parts.

## 7. Not in v1

Shadow DOM, slots, `::slotted()`, `:host`/`:host-context()`, `exportparts`, and a `part` prop that
re-exports a nested component's parts. A component is an ordinary scene subtree; parts are the only
selector form that crosses its boundary.

## 8. Tests

`tests/unit/part_pseudo_tests.cpp` (CTest `part_pseudo`): `parsing`, `specificity`, `matching`
(across the host boundary, nested components, no host), `cascade`, `diagnostics`, `supports_queries`,
`blob_round_trip`. `tests/unit/part_attribute_tests.cpp` (CTest `part_attribute`) covers
JSX → IR → `Node::set_part`, a dynamic `part` rejection and `set_component_name` on hosts.
