# Other elements: Image, Path, Spacer, Scroll, VirtualList, VirtualGrid, ErrorBoundary, CustomElement

Part of [`jsx-intrinsics-v1`](README.md). Notation: README section 2. Common attributes apply as
stated in [`common-and-events.md`](common-and-events.md) section 1 (its "Not on" column).

## 1. `Image`, `Path`, `Spacer`

Their props, types, required rules and diagnostics are normative in
[`jsx-subset-v1.md`](../jsx-subset-v1.md) section 11.1 and are not copied. This table adds the
lowering and the cases that section does not state.

| Element | Props (11.1) | IR | Children | Events | Ref | `className`/`style` |
|---|---|---|---|---|---|---|
| `Image` | `src` (L, required), `alt` (E, required), `fit` (L, default `"contain"`) | `props.src`, `props.alt`, `props.fit` | none | none | `Image` | yes |
| `Path` | `d` (L, required), `viewBox` (L, required), `fill`, `stroke` (L, default `"none"`), `strokeWidth` (L, default `1`), `label` (E) | `props.d`, `props.viewBox`, `props.fill`, `props.stroke`, `props.strokeWidth`, `props.label` | none | none | `Path` | yes |
| `Spacer` | `size` (length), `grow` (L, default `0`); one of the two | `props.size`, `props.grow` | none | none | none | no |

- Implied roles: `Image` is `img` (`alt=""` makes it decorative), `Path` is `img` only when `label` is
  present, `Spacer` is always decorative.
- `Image.alt` is an E value of type `string` but must still be present; `alt` absent is
  `TESSERA_JSX_IMAGE_ALT` (11.1).
- All three lower to leaf nodes; any child is `TESSERA_JSX_CHILDREN_NOT_ALLOWED`.
- `Spacer.size` as a number lowers to the bare number (dp); as a string it keeps the unit text.
- Aliases that map here: `img` ([`html-aliases.md`](html-aliases.md)).

## 2. `Scroll`

IR kind `Scroll`. The only built-in scrollable container
([`scroll-and-gestures-v1.md`](../scroll-and-gestures-v1.md) section 1). Implied role `scroll view`.

| Prop | Type | Req | Default | Value | IR | Notes |
|---|---|---|---|---|---|---|
| `axis` | `"y"`, `"x"`, `"both"` | no | `"y"` | L | `props.axis` | Scroll axes [Q10] |

Children: nodes only; text runs are `TESSERA_JSX_CHILDREN_NOT_ALLOWED` (subset 10.4). Events:
`onScroll` (`ScrollEvent`), the six pointer events. Ref: `Scroll` (needed for `scroll_to` and
`scroll_to_key`, which are developer C++ in V1). `scroll-behavior`, `overscroll-behavior`,
`overscroll-effect` and `scrollbar-gutter` are style properties
([`stylesheet-v1.md`](../stylesheet-v1.md) section 11), not props. Snap, physics and sticky/anchoring
configuration have no JSX form in V1 ("configured through `ScrollSnap`" names an API, not a prop)
[Q10]. `overflow` on `View` does not create a scroll container; use `Scroll`
([`react-compat.md`](react-compat.md) section 4).

## 3. `VirtualList` and `VirtualGrid`

IR kinds `VirtualList`, `VirtualGrid`; semantics in [`virtualization-v1.md`](../virtualization-v1.md).
Implied role `list`.

### 3.1 JSX shape [Q9]

The virtualization spec shows only IR. The JSX form chosen here reuses the loop form of subset 10.5, so
no function-valued prop is needed (subset section 9 forbids it):

```tsx
<VirtualList itemSize={32} gap={4} onEndReached={loadMore}>
  {rows.map((row) => (
    <RowView key={row.id} label={row.label} />
  ))}
</VirtualList>
```

- Children: exactly one child, an expression of the list-loop form of subset 10.5. Any other child
  (text, element, second child, fragment) is `TESSERA_JSX_CHILDREN_COUNT`. The callback has one or two
  parameters; its body is one element (intrinsic or component) with `key`; nested loops count toward the
  depth limit of 2.
- Lowering: `props.data` is the receiver of `.map` (a list `ref` or static list); the callback
  element is the item template with `item` and `index` as locals; `key` is the key Expression.
  The template is stored as the node's `itemTemplate` (ir-v1 section 3.5, which now exists in the IR
  model, writer and validator). The lowering that produces it is not implemented yet: the front end
  still rejects `VirtualList`/`VirtualGrid` with `TESSERA_IR_UNSUPPORTED`.
- The template is static: no `condition` on it, no dynamic component type.

### 3.2 Props

| Prop | Type | Req | Default | Value | IR | On | Notes |
|---|---|---|---|---|---|---|---|
| `itemSize` | length | one-of size | - | E | `props.itemSize` | `VirtualList` | Fixed main-axis size; O(1) range |
| `estimateSize` | length | one-of size | - | E | `props.estimateSize` | both | Variable size; default estimate is unspecified ([`virtualization-v1.md`](../virtualization-v1.md) section 13), so one of the two is required |
| `rowHeight` | length | one-of size (grid) | - | E | `props.rowHeight` | `VirtualGrid` | Fixed row height |
| `columns` | int | one-of cols | - | E | `props.columns` | `VirtualGrid` | Fixed column count (at least 1) |
| `columnWidth` | length | one-of cols | - | E | `props.columnWidth` | `VirtualGrid` | Fixed column width |
| `overscan` | length | no | `ui.virtualListOverscanPx` (200) | E | `props.overscan` | both | dp realized beyond the viewport ([`buffer-config-v1.md`](../buffer-config-v1.md)) |
| `gap` | length | no | `0` | E | `props.gap` | both | Between items (and between grid rows and columns) |
| `orientation` | `"vertical"`, `"horizontal"` | no | `"vertical"` | L | `props.orientation` | both | |
| `stickyHeaders` | bool | deferred | - | - | - | both | No JSX form marks a header, so the prop is not accepted in V1 [Q9] |

Constraints: `one-of size` is exactly one of `itemSize`/`estimateSize` on `VirtualList`, exactly one
of `rowHeight`/`estimateSize` on `VirtualGrid` (`TESSERA_JSX_PROP_ONE_OF`); `one-of cols` is exactly one
of `columns`/`columnWidth` (`TESSERA_JSX_PROP_ONE_OF`). Using a grid-only prop on `VirtualList`, or the
reverse, is `TESSERA_JSX_SUBSET_UNKNOWN_PROP`.

Events: `onEndReached` (no payload; throttled; UI thread), `onScroll` (`ScrollEvent`). Ref:
`VirtualList` / `VirtualGrid` (for `scroll_to_key`).

## 4. `ErrorBoundary`

IR kind `ErrorBoundary`; semantics in [`error-boundaries-v1.md`](../error-boundaries-v1.md). Not
painted, no layout effect, so no `className`, `style`, `id`, `role`, `aria-*` and no `ref`.

| Prop | Type | Req | Default | Value | IR | Notes |
|---|---|---|---|---|---|---|
| `fallback` | one element | yes | - | N | node field `fallback` | An intrinsic or component element. Not a conditional, loop, fragment or text. It may not contain the same boundary. The attribute value form `{<X />}` is new to the subset ([Q8]) |
| `onError` | handler of `ErrorInfo` | no | diagnostics only | H | `events.onError` | See below |
| `resetKey` | string, int or bool | no | - | E | `props.resetKey` | Any change resets the boundary |
| `maxAutoRetries` | int | no | `0` | L | `props.maxAutoRetries` | At least 0; bounded and logged |
| `catchEvents` | bool | no | `true` | L | `props.catchEvents` | |

- Children: exactly one element (intrinsic or component), not text, not a fragment, not a conditional
  or loop (a missing child would break "exactly one"): `TESSERA_JSX_CHILDREN_COUNT`. Wrap siblings in a
  `View`.
- `onError` takes the three handler forms. The examples of the error-boundary spec,
  `onError={(error) => report(error)}`, call a function that is not a setter, which subset 9.2 forbids;
  in V1 that handler must be a native handler (`bind::`) or a setter of an `ErrorInfo` state [Q8].
- Events: `onError` only. Ref: rejected.

## 5. `CustomElement`

IR kind `CustomElement`; manifest `ui/elements.json`
([`custom-elements-v1.md`](../custom-elements-v1.md)). Leaf.

| Attribute | Type | Req | Value | IR | Notes |
|---|---|---|---|---|---|
| `element` | string | yes | L | node field `element` | An `id` of `ui/elements.json` (`TESSERA_JSX_CUSTOM_ELEMENT_UNKNOWN`). No expression |
| `props` | object literal | no | L (keys), E (values) | each key becomes `props.<key>` | Keys are manifest props; types are manifest types (`bool`, `int`, `float`, `string`, `color`, `vec2`, `vec3`, `vec4`, `mat4`, `texture`, `buffer`, `node`); no unknown key, no spread, no missing required key (`TESSERA_JSX_PROP_REQUIRED`) |
| manifest events | handler | no | H | `events.on<Name>` | `select` becomes `onSelect`; the payload record is generated from the manifest `payload` |
| `onPointer`, `onKey`, `onWheel`, `onText`, `onGamepad` | handler | no | H | `events.<name>` | Allowed only when the manifest `input` list names `pointer`, `key`, `wheel`, `text`, `gamepad` respectively; payload is `ElementEvent`-derived record, defined by the manifest generator |

Children: none. `className`, `style`, `key`, `ref` (`CustomElement`), `id`, `role`, `aria-*` and
`automationId` follow the common table. The accessibility defaults come from the manifest `semantics`
and can be overridden by `role` and `aria-*`. The `props` attribute is flattened into node `props` as
shown in ir-v1 section 3.2, so a manifest prop must not be named like a reserved attribute
(`key`, `ref`, `className`, `style`, `id`, `role`, `element`, `props`, `aria-*`, `on*`); the manifest
validator rejects that.

The example of `custom-elements-v1.md` section 4 uses `selected ?? "none"` and `useState<number | null>`,
which the subset rejects (`??`, bare `number`); the example needs a correction [Q23].
