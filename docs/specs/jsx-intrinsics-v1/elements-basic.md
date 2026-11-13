# Basic elements: View, Text, Button, TextInput, TextArea

Part of [`jsx-intrinsics-v1`](README.md). Common attributes (`key`, `ref`, `className`, `style`, `id`,
`role`, `aria-*`, `automationId`) apply to all five and are not repeated; events use the payload
records of [`common-and-events.md`](common-and-events.md) section 2.

## 0. Text children: one rule for View, Text and Button

This section extends [`jsx-ir-lowering.md`](../../design/jsx-ir-lowering.md) decision 11 (accepted);
it does not change it.

- A **run** is JSX text and `{expr}` children of type `string`, `int` or `float` with no element in
  between. JSX whitespace and entities are already normalized by the parser (subset 10.4). `{true}`,
  `{false}`, `{null}` produce nothing and do not break a run.
- A run lowers to a **text Expression**: a `literal` string when it has no `{expr}`, otherwise a
  `template` whose parts are strings and Expressions (adjacent strings merged; an `int` or `float`
  part prints as in ir-v1 section 4.1). A single `{count}` is a template with one part, so the
  result is always `string`.
- `Text`: all children are one run; the run is `Text.text`.
- `Button` whose children are **only** text (one run): the run is `props.label` (decision 11).
- `Button` with any element child (including `{c && <X/>}` and `.map`): there is no `label`; each run
  becomes a synthetic `Text` child spanning the run, in source order (decision 11).
- `View`: each run becomes a synthetic `Text` child spanning the run (decision 11). Synthetic nodes
  have no `className`, `style` or events and inherit text style from the parent.
- Anywhere else (`Scroll`, `Spacer`, leaf elements, `ErrorBoundary`): a run is
  `TESSERA_JSX_CHILDREN_NOT_ALLOWED` (subset 10.4, last bullet).
- Numbers and strings only: `{cond && "x"}` is a type error because `&&` takes booleans
  (subset 8.2); use `{cond ? "x" : ""}`.

## 1. `View`

IR kind `View`. Container; lays out its children with the style engine (Yoga). Own props: `tabIndex`, `autoFocus`, `focusTrap`, `focusGroup`, `typeahead` (focusable and focus scope, [common-and-events.md](common-and-events.md) section 4).

| Aspect | Rule |
|---|---|
| Children | nodes and text runs (section 0). Fragments splice; conditionals and `.map` allowed |
| Props | `tabIndex` (`0` or `-1`), `autoFocus`, `focusTrap`, `focusGroup`, `typeahead` |
| Implied role | `generic` |
| Events | `onClick`; `onPointerDown`, `onPointerUp`, `onPointerMove`, `onPointerEnter`, `onPointerLeave`, `onPointerCancel` (payloads `ClickEvent`, `PointerEvent`); `onSizeChange` (`SizeChangeEvent`, [common-and-events.md](common-and-events.md) section 2); `onKeyDown`, `onKeyUp` (`KeyEvent`), `onFocus`, `onBlur` (`FocusEvent`), the last four on a focusable node |
| Ref | `View` |

A `View` with `tabIndex` must declare `role` and have an accessible name, else
`TESSERA_JSX_FOCUSABLE_NO_ROLE` / `TESSERA_JSX_FOCUSABLE_NO_NAME` (errors,
[`accessibility-v1.md`](../accessibility-v1.md) G-18). A `View` with only an event and no `role`
keeps the warning `TESSERA_JSX_A11Y_NO_ROLE`. No role is inferred. Evidence: `focusable_view`.

## 2. `Text`

IR kind `Text`.

| Aspect | Rule |
|---|---|
| Children | one run (section 0) or nothing. An element child is `TESSERA_JSX_CHILDREN_NOT_ALLOWED` (inline rich text is outside V1, [`text-editing-v1.md`](../text-editing-v1.md) section 1) with the suggestion to place two `Text` in a flex `View` |
| IR | `text` = the text Expression; `<Text />` is `text: ""` |
| Props | `tabIndex` (`0` or `-1`), `autoFocus` |
| Implied role | `text` |
| Events | as `View` |
| Ref | `Text` |

```tsx
<Text>Count: {count}</Text>
```

lowers to `"text": { "kind": "template", "parts": ["Count: ", { "kind": "ref", "id": "state::count" }] }`.

## 3. `Button`

IR kind `Button`. Implied role `button`; focusable; activates with pointer, Enter/Space and the
accessibility `activate` action, all delivered as `onClick`.

| Prop | Type | Req | Default | Value | IR | Notes |
|---|---|---|---|---|---|---|
| `label` | string | one-of content | - | E | `props.label` | Visible text. `label` is not a translation key [Q20] |
| `disabled` | bool | no | `false` | E | `props.disabled` | Not focusable, no click/pointer/key events, `:disabled` pseudo-state ([`component-model-v1.md`](../component-model-v1.md) section 7), accessibility state disabled |

Children: text runs and nodes. Events: `onClick`, the six pointer events, `onKeyDown`, `onKeyUp`,
`onFocus`, `onBlur`. Ref: `Button`.

Constraint `content`: a Button has visible content from exactly one of `label` or children, and
needs an accessible name from `aria-label`, `label`, or its content.

| Source | Result |
|---|---|
| `<Button>Increment</Button>` (text only) | `props.label = "Increment"`; no children |
| `<Button><Image src="icons/save.png" alt="" />Save</Button>` (node + text) | no `label`; children: `Image` node, then synthetic `Text("Save")`. The name comes from the text |
| `<Button label="Save" />` | `props.label = "Save"` |
| `<Button label="Save">Save</Button>` (both) | error `TESSERA_JSX_BUTTON_CONTENT_CONFLICT`: two sources of visible content. `aria-label` with children is allowed, since it is not visible content |
| `<Button>{count}</Button>` (dynamic text) | `props.label = { "kind": "template", "parts": [ref state::count] }` |
| `<Button>Count: {n}</Button>` (multi-part) | `props.label = { "kind": "template", "parts": ["Count: ", ref n] }` |
| `<Button />` (no label, no children) | error `TESSERA_JSX_BUTTON_EMPTY`, unless `aria-label` is present (then it is accepted: content comes from style, for example a background image) |
| `<Button>{busy && <Spinner />}Save</Button>` | element child present, so no `label`: children are the `Spinner` `Component` node (with `condition`) and synthetic `Text("Save")` |
| `<Button disabled>Save</Button>` | `props.disabled = true`, `props.label = "Save"` |
| `<Button><Image src="x.png" alt="Close" /></Button>` | no text; name from the `Image` `alt` |
| `<Button><Image src="x.png" alt="" /></Button>` | warning `TESSERA_JSX_BUTTON_NO_NAME` (empty derived name; add `aria-label`) [Q25] |

Name derivation order: `aria-label`, then `label`, then the concatenated text and `alt` of
descendants in order. `Button type=...` is not a prop of `Button`; it exists on the alias only
([`html-aliases.md`](html-aliases.md) section 3).

## 4. `TextInput` and `TextArea`

IR kinds `TextInput` and `TextArea`. Leaf intrinsics (`children` empty); semantics in
[`text-editing-v1.md`](../text-editing-v1.md). Implied role `textbox`; focusable.

### 4.1 Props

| Prop | Type | Req | Default | Value | IR | On | Notes |
|---|---|---|---|---|---|---|---|
| `value` | string | no | - | E | `props.value` | both | Controlled value; source of truth is app state. Type is exactly `string` |
| `defaultValue` | string | no | `""` | E | `props.defaultValue` | both | Uncontrolled initial value, read at mount only; later changes are ignored (as in React) |
| `placeholder` | string | no | - | E | `props.placeholder` | both | Never a value, never a label |
| `readOnly` | bool | no | `false` | E | `props.readOnly` | both | Focus, select and copy only |
| `disabled` | bool | no | `false` | E | `props.disabled` | both | No focus, no events |
| `secure` | bool | no | `false` | E | `props.secure` | `TextInput` | Password semantics and redaction; on `TextArea` it is `TESSERA_JSX_SUBSET_UNKNOWN_PROP` [Q24] |
| `maxLength` | int | no | no limit | E | `props.maxLength` | both | Counts graphemes. An edit or paste that would exceed it is rejected as a whole, not truncated ([`text-editing-v1.md`](../text-editing-v1.md) section 10.1); this differs from the browser's paste truncation. A negative literal is an error |
| `inputMode` | enum | no | `"text"` | L | `props.inputMode` | both | `"text"`, `"numeric"`, `"email"`, `"url"`, `"search"`, `"tel"` |
| `autofill` | enum | no | - | L | `props.autofill` | both | Platform autofill hint; vocabulary in [Q19] |
| `selection` | `TextSelection` | no | - | E | `props.selection` | both | Controlled selection |
| `scrollPolicy` | enum | no | `"caret"` | L | `props.scrollPolicy` | both | `"caret"`, `"none"`, `"always"` |
| `rows` | int | no | `2` [Q19] | E | `props.rows` | `TextArea` | Visible lines, at least 1. On `TextInput` it is `TESSERA_JSX_SUBSET_UNKNOWN_PROP` |
| `wrap` | enum | no | `"word"` [Q19] | L | `props.wrap` | `TextArea` | `"word"`, `"none"` |

Constraints:

| Id | Rule | Code |
|---|---|---|
| `value-xor-default` | `value` and `defaultValue` together are an error: a field is controlled or uncontrolled, never both ([`component-library.md`](../../design/component-library.md) section 4: no mixed ownership) | `TESSERA_JSX_INPUT_VALUE_BOTH` |
| `controlled-needs-onchange` | `value` without `onChange`, `readOnly` or `disabled` is a warning, not an error: the field would apply an edit optimistically and then restore the previous value on the next sync [Q4] | `TESSERA_JSX_INPUT_NO_ONCHANGE` |
| `no-children` | children of any kind (including text) are an error; React's `<textarea>text</textarea>` is not supported, use `defaultValue` | `TESSERA_JSX_CHILDREN_NOT_ALLOWED` |
| `value-type` | `value`/`defaultValue` of a type other than `string` (for example an `int` state) is a type error; build a string with a template literal ``{`${n}`}`` | `TESSERA_JSX_TYPE_MISMATCH` |

### 4.2 `TextInput` and `TextArea` differences

`TextArea` is multi-line: Enter inserts a line break, `rows` and `wrap` exist, the caret is scrolled
into view through an internal `Scroll` ([`text-editing-v1.md`](../text-editing-v1.md) section 10.1).
`TextInput` is single-line: it never wraps, Enter fires `onSubmit`, and it alone has `secure`. Every
other prop, constraint and event is shared. Both accept `placeholder`, `maxLength`, `inputMode`.

### 4.3 Events

| Event | Payload | Fires | Notes |
|---|---|---|---|
| `onChange` | `TextChangeEvent` | after each committed edit, once per command | not while composing; the composition commit fires once |
| `onSubmit` | `TextSubmitEvent` | `TextInput`: Enter. `TextArea`: trigger not specified [Q5] | |
| `onFocus`, `onBlur` | `FocusEvent` | focus changes | |
| `onKeyDown`, `onKeyUp` | `KeyEvent` | before default editing commands | the handler cannot cancel the default (`preventDefault` is native-only, [Q3]) |
| `onSelectionChange` | `TextSelection` | selection or caret moved | |
| `onCompositionStart`, `onCompositionUpdate`, `onCompositionEnd` | `CompositionEvent` | IME | |

`onChange` forms (payload rules in [`common-and-events.md`](common-and-events.md) section 3):

| Form | Payload the handler sees | Accepted when |
|---|---|---|
| `onChange={setEmail}` (setter reference) | the setter receives `value` (a `string`) | state type is `string` |
| `onChange={(e) => setEmail(e.target.value)}` | `e: TextChangeEvent` | arguments type-check |
| `onChange={(e) => setEmail(e.value)}` | `e: TextChangeEvent` | arguments type-check |
| `onChange={() => setDirty(true)}` | none used | always |
| native: `// Note: bind::on_email` then `const f = (e: TextChangeEvent): void => {...}` | `e: TextChangeEvent` | annotation is exactly the record name |

```tsx
<TextInput
  value={email}
  onChange={setEmail}
  placeholder="Email"
  inputMode="email"
  autofill="email"
  maxLength={254}
  onSubmit={() => setSent(true)}
/>
<TextArea value={notes} onChange={(e) => setNotes(e.target.value)} rows={6} readOnly={locked} />
```

Not supported until the allowlist (ir-v1 section 4.2) gains the decided total functions
`parseIntOr(s, fallback)` and `parseFloatOr(s, fallback)` [Q26]: numeric state fed from an input
(`setAge(parseInt(e.target.value))`). Until then keep the string.
