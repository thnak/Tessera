# Common attributes, event payloads and handler rules

Part of [`jsx-intrinsics-v1`](README.md). Notation: see section 2 of the README.

## 1. Common and reserved attributes

Accepted on every intrinsic unless the "Not on" column says otherwise. The set of reserved names is
that of subset spec 10.2 and [`component-model-v1.md`](../component-model-v1.md) section 3.

| Attribute | Type | Value | Default | IR | Not on | Rules |
|---|---|---|---|---|---|---|
| `key` | string or int | E | - | `key` | none | Required on the root element of a `.map` callback; elsewhere rejected (subset 10.2). `key` on a list template that is a `Component` node is the same rule. Not `bool`, not `float`. React's use of `key` to force a remount outside lists is not supported ([`react-compat.md`](react-compat.md) section 4) |
| `ref` | name of a `useRef<T>` | name | - | `refs[]` | `Spacer`, `ErrorBoundary` | `T` must equal the element type column of README section 3 (for aliases: the table of `html-aliases.md` section 4). Callback refs (`ref={fn}`) and object refs not from `useRef` are rejected (`TESSERA_JSX_SUBSET_FUNCTION_VALUE`). The members of a ref are not specified in V1 [Q6]; in V1 a ref is used only by developer C++ in native handlers and effects |
| `className` | string | E | - | `props.className` | `Spacer`, `ErrorBoundary` | Space-separated class names, checked against the project stylesheets (unknown class is a warning, subset 10.3). `class` is not accepted ([`react-compat.md`](react-compat.md) section 4) |
| `style` | object literal | L (keys), E (values) | - | `style` | `Spacer`, `ErrorBoundary` | Subset 10.3. A `style` object that is not an inline literal is `TESSERA_JSX_SUBSET_STYLE_DYNAMIC` |
| `id` | string | L | - | `props.id` | `Spacer`, `ErrorBoundary` | Selector target (`#id`) and nothing else in V1: it is not a label target and not an automation id [Q7]. Duplicate literals in one component are a warning |
| `role` | enum literal | L | per element | `props.role` | `Spacer`, `ErrorBoundary` | One of the tokens of the table in section 1.1; an unknown token is `TESSERA_JSX_SUBSET_UNKNOWN_PROP` with the token list as suggestion |
| `aria-label` | string | E | - | `props["aria-label"]` | `Spacer`, `ErrorBoundary` | The accessible name; overrides a derived name |
| `aria-hidden` | bool | E | `false` | `props["aria-hidden"]` | `Spacer`, `ErrorBoundary` | Leaves the node and its subtree out of the accessibility tree |
| `aria-disabled`, `aria-selected`, `aria-expanded`, `aria-busy`, `aria-invalid`, `aria-readonly`, `aria-required` | bool | E | `false` | `props["aria-..."]` | same | The state of the same name in [`accessibility-v1.md`](../accessibility-v1.md) section 2 |
| `aria-checked` | `"true"`, `"false"`, `"mixed"` | E | - | `props["aria-checked"]` | same | `checked/mixed` state |
| `automationId` | string literal, decimal uint64 | L | - | `automationId` | none | Must equal the computed `uiId` (ir-v1 section 3.1); a mismatch is a validator error |

Any other `aria-*` name is `TESSERA_JSX_SUBSET_UNKNOWN_PROP` in V1 (the V1 set is closed so that every
accepted name has a defined effect; `aria-labelledby`, `aria-describedby`, `aria-controls` need an id
relation that no spec defines [Q7]). `data-*`, `title`, `hidden`, `tabIndex`, `accessKey`,
`contentEditable`, `draggable`, `dangerouslySetInnerHTML` are rejected (subset 10.2 and
[`react-compat.md`](react-compat.md) section 4).

### 1.1 Role tokens

Roles are written as WAI-ARIA tokens, because React code already uses them. They map to the roles of
[`accessibility-v1.md`](../accessibility-v1.md) section 2.

| Token | Accessibility role | Token | Accessibility role |
|---|---|---|---|
| `generic`, `presentation`, `none` | generic (the last two also hide the role) | `switch` | switch |
| `button` | button | `slider` | slider |
| `link` | link | `progressbar` | progress |
| `heading` | heading | `list` | list |
| `img` | image | `listitem` | list item |
| `textbox` | text field | `tab` | tab |
| `checkbox` | checkbox | `tabpanel` | tab panel |
| `radio` | radio | `dialog`, `menu`, `menuitem`, `tooltip`, `alert` | same name |

`text` and `scroll view` have no token: they are the implied roles of `Text` and `Scroll`. Each
intrinsic has an implied role (`Button` is `button`, `TextInput`/`TextArea` are `textbox`, `Image`
is `img`, `Text` is `text`, `Scroll` is `scroll view`, others `generic`); an explicit `role` replaces
it. The exact vocabulary belongs to the accessibility spec; this table is the compile-time list [Q7].

## 2. Event payload records

Each event passes one payload. These are record types ([`jsx-subset-v1.md`](../jsx-subset-v1.md)
section 8.2: member access is allowed on a record whose type is known). They are declared once in the
framework typings (`.d.ts`, subset 16.2) and in the table of [`machine-readable.md`](machine-readable.md).
The field lists are the proposal of this spec; the normative event model is
[`input.md`](../../design/input.md) section 3, which does not yet list per-event fields [Q2].

| Record | Fields (all required) | Used by |
|---|---|---|
| `PointerEvent` | `x: float`, `y: float` (node-local logical px), `pointerId: int`, `pointerType: "mouse" \| "touch" \| "pen"`, `button: int` (0 left, 1 middle, 2 right, 3 back, 4 forward, 5 other), `shift: bool`, `ctrl: bool`, `alt: bool`, `meta: bool` | `onPointerDown/Up/Move/Enter/Leave/Cancel` |
| `ClickEvent` | the fields of `PointerEvent`, but `pointerType` also allows `"keyboard"` and `"assistive"`; for those, `x`,`y` are the node center and `button` is `0` | `onClick` |
| `KeyEvent` | `key: string` (W3C key value: `"Enter"`, `"Escape"`, `"a"`), `code: string`, `repeat: bool`, `shift`, `ctrl`, `alt`, `meta: bool` | `onKeyDown`, `onKeyUp` |
| `FocusEvent` | no fields | `onFocus`, `onBlur` |
| `TextChangeEvent` | `value: string`, `target: TextTarget` | `onChange` |
| `TextSubmitEvent` | `value: string`, `target: TextTarget` | `onSubmit` of `TextInput`/`TextArea` |
| `TextTarget` | `value: string` | member `target` of the two records above |
| `TextSelection` | `anchor: int`, `focus: int` (grapheme offsets, [`text-editing-v1.md`](../text-editing-v1.md) section 2; affinity is not exposed) | `onSelectionChange` and the `selection` prop |
| `CompositionEvent` | `text: string`, `start: int`, `end: int` | `onCompositionStart/Update/End` |
| `ScrollEvent` | `offsetX: float`, `offsetY: float`, `viewportWidth: float`, `viewportHeight: float`, `contentWidth: float`, `contentHeight: float` | `onScroll` |
| `SizeChangeEvent` | `width: float`, `height: float`, `contentWidth: float`, `contentHeight: float` (logical px; border box and content box, [layout observation](../../design/layout-observation.md) section 6.1) | `onSizeChange` |
| `ErrorInfo` | `code: string`, `message: string` | `onError` of `ErrorBoundary` ([`error-model-v1.md`](../error-model-v1.md) `Error`, reduced to the two fields that are safe to show) |

`value` and `target.value` of `TextChangeEvent` are always equal; `value` is the Tessera form and
`target.value` the React form ([`react-compat.md`](react-compat.md) section 2, [Q1]). A payload
of a secure field carries the real text to the handler (the handler is the app's own code); the
redaction rules of [`text-editing-v1.md`](../text-editing-v1.md) section 10 apply to logs and snapshots.

## 3. Handler rules for events

1. A handler is one of the three forms of subset spec section 9 and nothing else.
2. A handler may declare fewer parameters than the payload, never more than one: `onClick={() => ...}`
   and `onClick={(e) => ...}` are both accepted; `(a, b)` is `TESSERA_JSX_TYPE_MISMATCH`.
3. **Inline setter arrow** (9.2): the parameter, if present, is typed by the event row and may be
   used in arguments through member access (`e.target.value`, `e.key`). Examples that are accepted:

   ```tsx
   <TextInput value={name} onChange={(e) => setName(e.target.value)} />
   <TextInput value={name} onChange={(e) => setName(e.value)} />
   <Button onClick={() => setCount(count + 1)}>Add</Button>
   ```

4. **Setter reference** (9.1): `onChange={setName}` passes the payload's **primary member** to the
   setter. The primary member of `TextChangeEvent` and `TextSubmitEvent` is `value`; of `TextSelection`
   the whole record; for every other record there is none, and a setter reference is then accepted only
   when the state type equals the payload record type. The state type must match the primary member
   type (`string` for text events), else `TESSERA_JSX_TYPE_MISMATCH` ([Q1]; this extends 9.1,
   see [`react-compat.md`](react-compat.md) section 3).
5. **Native handler** (9.3): the first parameter, if present, is annotated with the payload record name
   imported from `"tessera"`: `(e: TextChangeEvent): void`. A different annotation (`string`) is
   `TESSERA_JSX_TYPE_MISMATCH`. `stopPropagation` and `preventDefault` exist only in the developer C++
   of the protected region; the TS record has no methods and the preview body cannot call them [Q3].
6. Event delivery is on the UI thread at tick boundaries and never blocks
   ([`component-model-v1.md`](../component-model-v1.md) section 4).
7. A `disabled` element does not deliver pointer, key, focus or change events; the handler stays in IR.

## 4. Events by element (summary)

The full rows are in the element tables. `P` = the six pointer events, `K` = `onKeyDown`/`onKeyUp`,
`F` = `onFocus`/`onBlur`, `S` = `onSizeChange` (every element with a layout box; delivered
by layout observation, not through event dispatch, so it neither bubbles nor captures).

| Element | Events |
|---|---|
| `View`, `Text` | `onClick`, `P`, `S` |
| `Button` | `onClick`, `P`, `K`, `F`, `S` |
| `TextInput`, `TextArea` | `onChange`, `onSubmit`, `K`, `F`, `onSelectionChange`, `onCompositionStart/Update/End`, `S` |
| `Scroll` | `onScroll`, `P`, `S` |
| `VirtualList`, `VirtualGrid` | `onEndReached`, `onScroll`, `S` |
| `ErrorBoundary` | `onError` (no `S`: it has no layout box) |
| `CustomElement` | the events of its manifest entry plus `onPointer`, `onKey`, `onWheel`, `onText`, `onGamepad` as the manifest `input` list allows, and `S` |
| `Image`, `Path` | `S` only |
| `Spacer` | none (no box of its own to observe; subset 11.1) |

Not defined in V1, so rejected as unknown: `onDoubleClick`, `onContextMenu`, `onWheel` on non-custom
elements, `onDrag*`, `onTouch*`, `onMouse*`, `onInput`, `onLongPress` and the gesture recognizers of
[`scroll-and-gestures-v1.md`](../scroll-and-gestures-v1.md) section 5, which name no handler props
[Q21]. `onEditCommand` (advanced) is deferred until `text-editing-v1.md` defines its record.
`onKeyDown`/`onFocus` on `View` are deferred until focusability of a `View` is specified [Q13].
