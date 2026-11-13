# Lowercase HTML tags and attributes (normative mapping)

Part of [`jsx-intrinsics-v1`](README.md). Decision: [ADR-039](../../../decisions/ADR-039-lowercase-html-tags-accepted.md).
This page is the only definition of the mapping; the data form is in
[`machine-readable.md`](machine-readable.md) section 3, and the table below is generated from or checked
against it.

## 1. Rules

1. A lowercase tag in section 2 resolves to the listed intrinsic. A lowercase tag in section 5, or in
   neither table, is `TESSERA_JSX_SUBSET_HTML_TAG`; the message names the tag, the suggestion is the
   `Alternative` text of section 5, or "use an intrinsic such as `<View>` or `<Text>`" when the tag is
   unknown. Component names stay uppercase (`TESSERA_JSX_SUBSET_COMPONENT_NAME`), so there is no collision.
2. **Alias is spelling only.** No default styles, sizes, margins, list markers or semantics beyond the
   `Injected` column are added. A browser renders `h1` large and bold, `ul` with markers and `div`
   as a block; the alias does not ([Q12]). Layout of a `View` is the style engine's, not the browser's.
   The one exception is the inline-emphasis group of section 2 (`b`, `strong`, `i`, `em`, `code`):
   those tags inject only the run style that names their emphasis, because a span with no visual
   difference would make the tag meaningless. The injected style is a default: an explicit
   `className`/`style` of the same property wins, and the non-metric rule of
   [`elements-basic.md`](elements-basic.md) section 2.1 still applies.
3. After resolution the element is the intrinsic: its prop table, children rule, constraints and event
   table in [`elements-basic.md`](elements-basic.md) / [`elements-other.md`](elements-other.md) apply,
   plus the HTML attribute rewrites of section 3. Attribute names use React spelling (`readOnly`,
   `maxLength`, `autoComplete`), never HTML spelling (`readonly`, `maxlength`).
4. Rewrites apply only to elements written with a lowercase tag. A capitalized intrinsic accepts only
   its own table (`<TextInput type="password">` is `TESSERA_JSX_SUBSET_UNKNOWN_PROP`).
5. Injected props are defaults: an explicit attribute of the same name wins (for example an explicit
   `role` on `li`).
6. The closing tag must match the opening tag as written (`<div></div>`, not `<div></View>`).
7. Resolution runs in analysis; the AST keeps the tag as written; the IR node `source` is the span of
   the element as written. Diagnostics point at the original attribute or tag.
8. Event props follow the intrinsic's table. React names that the table lacks (`onMouseDown`,
   `onDoubleClick`, `onInput`, `onKeyPress`, ...) are `TESSERA_JSX_SUBSET_UNKNOWN_PROP` with a
   suggestion where one exists (`onMouseDown` to `onPointerDown`, `onInput` to `onChange`) [Q21].

## 2. Tag table

| HTML tag | Intrinsic | Injected | Status | Notes |
|---|---|---|---|---|
| `div`, `section`, `article`, `main`, `header`, `footer`, `nav`, `aside`, `figure` | `View` | - | mapped | Landmark roles are not in the accessibility role list, so none is injected |
| `ul`, `ol` | `View` | `role="list"` | mapped | No list markers and no numbering |
| `li` | `View` | `role="listitem"` | mapped | |
| `span`, `p`, `label`, `figcaption` | `Text` | - | mapped | `label` does not focus its control; `htmlFor` is rejected [Q11]. As a child of `Text`, `span` is an inline span ([`elements-basic.md`](elements-basic.md) section 2.1) |
| `b`, `strong` | `Text` | `style.font-weight: 700` | mapped | Inline span inside `Text`; the injected weight is a default, not a wrapper style |
| `i`, `em` | `Text` | `style.font-style: italic` | mapped | Inline span inside `Text`; default style as for `b` |
| `code` | `Text` | `style.font-family: monospace` | mapped | Inline span inside `Text`; the V1 text stack paints one app face, so the family is recorded but changes no metrics (no font selection yet) |
| `h1`, `h2`, `h3`, `h4`, `h5`, `h6` | `Text` | `role="heading"` | mapped | The level is not exposed in V1; not an inline span |
| `a` | `Text` | `role="link"` | partial | `href`, `target`, `rel` are rejected; use `onClick`. See [Q11]. As a child of `Text` it is an inline clickable span |
| `button` | `Button` | - | mapped | `type` rule in section 3.1 |
| `input` | `TextInput` | by `type` | mapped | Section 3.2 |
| `textarea` | `TextArea` | - | mapped | Section 3.3 |
| `img` | `Image` | - | mapped | Section 3.4 |

The emphasis rows inject a **default run style**, not a wrapper element style: a user `className` or
`style` of the same property wins, and only the properties of [`elements-basic.md`](elements-basic.md)
section 2.1 are accepted on a span. The `p`, `label`, `figcaption` and heading aliases are block-ish in
a browser but are plain `Text` nodes here (no default style, no inline-span default): only a child of
`Text` becomes an inline span.

## 3. Attribute rewrites

Attributes not mentioned here follow the intrinsic's table unchanged. An HTML attribute that has no
V1 meaning is `TESSERA_JSX_SUBSET_HTML_ATTR` with the alternative in the table; the same code applies
to attributes that exist on no intrinsic (`class`, `accessKey`, ...).

### 3.1 All aliases and `button`

| Attribute | Result |
|---|---|
| `className`, `style`, `id`, `key`, `ref`, `role`, `aria-*`, event props | unchanged (common table) |
| `class` | `HTML_ATTR`: write `className` |
| `accessKey`, `title`, `lang`, `dir`, `draggable`, `contentEditable`, `data-*` | `HTML_ATTR`: no V1 meaning; `title` is not a tooltip (use the overlay service or `aria-label`) |
| `hidden` | `HTML_ATTR`: use `{cond && <X />}` or `style={{ display: "none" }}` |
| `button type="button"` | accepted, dropped |
| `button type="submit"` or `"reset"` | `HTML_ATTR`: V1 has no `form` element; use `onClick` |
| `disabled` | unchanged (`props.disabled`) |

### 3.2 `input` (to `TextInput`)

`type` must be a string literal (`TESSERA_JSX_PROP_NOT_LITERAL`) and is removed; it injects:

| `type` | Injected on `TextInput` | Note |
|---|---|---|
| absent, `"text"` | - | |
| `"password"` | `secure` | |
| `"email"`, `"tel"`, `"url"`, `"search"` | `inputMode` of the same name | No validation |
| `"number"` | `inputMode="numeric"` | The value stays a `string`; no number parsing, no min/max/step ([Q26]) |
| anything else (`checkbox`, `radio`, `range`, `file`, `date`, `time`, `color`, `hidden`, `submit`, `button`, `reset`, ...) | none: `TESSERA_JSX_SUBSET_HTML_INPUT_TYPE` | `checkbox`/`radio`: library `Checkbox`, `Radio` ([`component-library.md`](../../design/component-library.md) section 3, after V1); `submit`/`button`: `<Button>` |

| Attribute | Result |
|---|---|
| `value`, `defaultValue`, `placeholder`, `readOnly`, `disabled`, `maxLength`, `inputMode` | unchanged |
| `autoComplete` | renamed `autofill`; `"off"` is dropped; tokens `email`, `username`, `current-password`, `new-password`, `one-time-code`, `name`, `tel`, `street-address`, `postal-code` pass; any other token is `HTML_ATTR` [Q19] |
| `onChange` | unchanged; payload per [`common-and-events.md`](common-and-events.md) section 2 |
| `checked`, `defaultChecked`, `name`, `required`, `pattern`, `min`, `max`, `step`, `minLength`, `multiple`, `accept`, `list`, `form`, `size` | `HTML_ATTR` (form and validation belong to the library `Form`/`Field`, [`forms-and-validation-v1.md`](../forms-and-validation-v1.md)) |
| `autoFocus`, `spellCheck`, `autoCapitalize`, `autoCorrect` | `HTML_ATTR`: not in V1 on inputs; `autoFocus` is available on a focusable `View` |

### 3.3 `textarea` (to `TextArea`)

| Attribute | Result |
|---|---|
| `value`, `defaultValue`, `placeholder`, `readOnly`, `disabled`, `maxLength`, `rows`, `onChange` | unchanged |
| `wrap` | `"soft"` becomes `"word"`, `"off"` becomes `"none"`, `"hard"` is `HTML_ATTR` |
| `cols` | `HTML_ATTR`: set a width in `style` |
| children text (`<textarea>x</textarea>`) | `TESSERA_JSX_CHILDREN_NOT_ALLOWED`: use `defaultValue` |

### 3.4 `img` (to `Image`)

| Attribute | Result |
|---|---|
| `src`, `alt` | unchanged; same rules as `Image` (project-relative literal path, `alt` required) |
| `width`, `height` | becomes `style.width` / `style.height`: a number or numeric string is dp, a string with a unit keeps it. Together with the same key in `style` it is `TESSERA_JSX_PROP_CONFLICT` |
| `srcSet`, `sizes`, `loading`, `decoding`, `crossOrigin`, `onLoad`, `onError` | `HTML_ATTR` |

### 3.5 `a`

`onClick` and the common attributes only. `href`, `target`, `rel`, `download` are `HTML_ATTR` with the
suggestion `onClick` [Q11]. As a child of `Text`, `a` is an inline span: the injected `role="link"`
plus `onClick` make the run a clickable, accessible link.

### 3.6 Inline spans (`span`, `b`, `strong`, `i`, `em`, `code`, `a` inside `Text`)

Inside `Text` these tags accept the `Text` attribute set: `className`, `style`, `role`, the `aria-*`
attributes, `onClick` and `key`. `id` is accepted as common metadata. `ref`, `tabIndex` and
`autoFocus` are rejected on a span (`TESSERA_JSX_SUBSET_HTML_ATTR`, suggestion: put them on the
parent `Text`): a span is not a box and has no focus order. No other attribute is rewritten; the
`a` rewrites of section 3.5 still apply. The span rules (static text, non-metric styles, hit testing,
accessibility, limits) are [`elements-basic.md`](elements-basic.md) section 2.1.

The same tags written outside `Text` remain plain `Text` elements (the emphasis defaults still
apply); only a `Text` child becomes an inline span.

## 4. DOM element types in `useRef`

In the type argument of `useRef` (subset 7), these names are accepted as the intrinsic on the right.
They are declared in the framework typings so that editors accept them; no import is needed in the
port, but writing one from `"tessera"` is also accepted.

| Name | Resolves to | Name | Resolves to |
|---|---|---|---|
| `HTMLDivElement`, `HTMLElement` | `View` | `HTMLButtonElement` | `Button` |
| `HTMLSpanElement`, `HTMLParagraphElement`, `HTMLHeadingElement`, `HTMLLabelElement`, `HTMLAnchorElement` | `Text` | `HTMLInputElement` | `TextInput` |
| `HTMLImageElement` | `Image` | `HTMLTextAreaElement` | `TextArea` |
| `HTMLUListElement`, `HTMLOListElement`, `HTMLLIElement` | `View` | | |

A ref has no `.current` in V1: `r.focus()` and `r.blur()` are the only members and are handler
statements (issue #226, [common-and-events.md](common-and-events.md) section 4). `r.current.focus()`
and every other member are rejected by subset section 9 ([Q6]).

## 5. Tags with no mapping

All of these are `TESSERA_JSX_SUBSET_HTML_TAG`; the suggestion is the Alternative.

| Tags | Alternative |
|---|---|
| `u`, `s`, `small`, `mark`, `kbd`, `sub`, `sup`, `abbr` | Inline rich text beyond the emphasis aliases is outside V1: a `Text` with `className` (for example a class that sets `font-weight`) |
| `br` | End the `Text` and start a new one in a column `View`, or put `\n` in a string expression (line breaking follows `white-space`) |
| `hr` | A `View` with a border style |
| `pre`, `blockquote`, `q`, `cite` | `Text` with `className` (`white-space: pre` for `pre`) |
| `form` | No `form` in V1. `TextInput` `onSubmit` plus `Button` `onClick`; the library `Form` and `Field` after V1 |
| `select`, `option`, `optgroup`, `datalist` | Library `Select` (after V1) |
| `table`, `thead`, `tbody`, `tfoot`, `tr`, `td`, `th`, `caption`, `colgroup`, `col` | `View` with flex or grid style; `VirtualGrid` or library `DataGrid` for large data |
| `svg`, `g`, `circle`, `rect`, `line`, `polyline`, `polygon`, `ellipse`, `text`, `defs`, `use`, `path` | `Path` with `d` and `viewBox` ([Q17] for an automatic single-path rewrite) |
| `canvas`, `video`, `audio`, `iframe`, `embed`, `object`, `picture`, `source`, `track` | `CustomElement` ([`custom-elements-v1.md`](../custom-elements-v1.md)) |
| `dialog`, `details`, `summary`, `menu`, `popover` | Overlay service and library components ([`overlays-and-layers-v1.md`](../overlays-and-layers-v1.md)) |
| `progress`, `meter` | Library `Progress` (after V1) |
| `html`, `head`, `body`, `title`, `meta`, `link`, `script`, `style`, `base`, `noscript`, `template`, `slot` | None: the app shell, `tessera.json` and `import "./file.css"` replace them |

## 6. Diagnostics

| Code | When | Severity |
|---|---|---|
| `TESSERA_JSX_SUBSET_HTML_TAG` | A lowercase tag with no mapping (section 5 or unknown). The code is kept for this case only | error |
| `TESSERA_JSX_SUBSET_HTML_ATTR` | An attribute with no V1 meaning on an alias or any intrinsic (new code) | error |
| `TESSERA_JSX_SUBSET_HTML_INPUT_TYPE` | `input type` outside section 3.2 (new code) | error |
| `TESSERA_JSX_PROP_CONFLICT` | `img width` with `style.width` and similar | error |

Both new codes carry a suggestion text with one text edit where the fix is mechanical (`class` to
`className`, `onMouseDown` to `onPointerDown`).
