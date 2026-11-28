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
2. **The alias adds no style of its own.** Its look comes from the user-agent stylesheet
   ([ADR-063](../../../decisions/ADR-063-browser-matching-user-agent-defaults.md),
   [`ua-audit.md`](../../reference/ua-audit.md)), which reproduces the browser defaults (`h1` large and bold, `div` a block, `ul` indented) and is overridden by any author rule.
   The one exception is the inline-emphasis group of section 2 (`b`, `strong`, `i`, `em`, `code`,
   and the ADR-059 group A tags `u`, `s`, `small`, `mark`, `kbd`, `sub`, `sup`, `cite`, `pre`):
   those tags inject only the run style that names their meaning, because a span with no visual
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
9. **The tag is kept for selectors.** Every alias also injects `tag` (the tag as written, `h1`, `p`, `a`, ...), so a type selector matches it as well as the carrier: a global `h1 { margin: 8px 0 }` styles the heading, and `p`, `a`, `ul`, `li` work the same way, as in a React + Vite project (issue #610). The carrier (`Text`, `View`, `Button`) still matches too. An explicit `tag` wins like any injected prop.
8. Event props follow the intrinsic's table. React names that the table lacks (`onMouseDown`,
   `onDoubleClick`, `onInput`, `onKeyPress`, ...) are `TESSERA_JSX_SUBSET_UNKNOWN_PROP` with a
   suggestion where one exists (`onMouseDown` to `onPointerDown`, `onInput` to `onChange`) [Q21].

## 2. Tag table

| HTML tag | Intrinsic | Injected | Status | Notes |
|---|---|---|---|---|
| `div`, `section`, `article`, `main`, `header`, `footer`, `nav`, `aside`, `figure` | `View` | - | mapped | Landmark roles are not in the accessibility role list, so none is injected |
| `ul`, `ol` | `View` | `role="list"` | mapped | The user-agent sheet gives the list its marker type (`disc`, `circle`, `square` by depth; `decimal` for `ol`); the items draw it ([`list-style.md`](../../design/list-style.md)). `ol start` and `reversed` become `aria-start` and `aria-reversed` (section 3.7) |
| `p` | `Text` | `role="paragraph"` | mapped | the role lets the user-agent sheet give it a margin ([`ua-audit.md`](../../reference/ua-audit.md)) |
| `li` | `View` | `role="listitem"` | mapped | Draws the inherited `list-style-type` marker outside its box, or inside the first line of its first `Text` for `list-style-position: inside` ([`list-style-inside.md`](../../design/list-style-inside.md)); numbered by position among the list's items, moved by `ol start`/`reversed` and reset by `value` (section 3.7) |
| `span`, `label`, `figcaption` | `Text` | - | mapped | `label` does not focus its control; `htmlFor` is rejected [Q11]. As a child of `Text`, `span` is an inline span ([`elements-basic.md`](elements-basic.md) section 2.1) |
| `b`, `strong` | `Text` | `style.font-weight: 700` | mapped | Inline span inside `Text`; the injected weight is a default, not a wrapper style |
| `i`, `em` | `Text` | `style.font-style: italic` | mapped | Inline span inside `Text`; default style as for `b` |
| `code` | `Text` | `style.font-family: monospace` | mapped | Inline span inside `Text`; the V1 text stack paints one app face, so the family is recorded but changes no metrics (no font selection yet) |
| `h1`, `h2`, `h3`, `h4`, `h5`, `h6` | `Text` | `role="heading"`, `aria-level="1"`-`"6"` | mapped | The level is `aria-level`; the user-agent sheet sizes it ([ua-audit.md](../../reference/ua-audit.md)); not an inline span |
| `a` | `Text` | `role="link"` | partial | `href`, `target`, `rel` are rejected; use `onClick`. See [Q11]. As a child of `Text` it is an inline clickable span |
| `button` | `Button` | - | mapped | `type` rule in section 3.1 |
| `input` | `TextInput` | by `type` | mapped | Section 3.2; `type="checkbox"`, `"radio"` and `"range"` lower onto library components (section 3.8) |
| `textarea` | `TextArea` | - | mapped | Section 3.3 |
| `img` | `Image` | - | mapped | Section 3.4 |
| `u` | `Text` | `style.text-decoration-line: underline` | mapped | Inline span inside `Text` ([`text-decoration.md`](../../design/text-decoration.md)) |
| `s` | `Text` | `style.text-decoration-line: line-through` | mapped | Inline span inside `Text` |
| `mark` | `Text` | `style.background-color: #ffff00`, `style.color: #000000` | mapped | Inline span inside `Text`; the span fills its run's line box |
| `kbd` | `Text` | `style.font-family: monospace` | mapped | Inline span inside `Text`; family recorded, no metric change (as `code`) |
| `cite` | `Text` | `style.font-style: italic` | mapped | Inline span inside `Text` |
| `abbr` | `Text` | - | mapped | Inline span inside `Text`; `title` is `HTML_ATTR`, so there is no dotted underline |
| `q` | `Text` | - | mapped | Lowering wraps the children in U+201C and U+201D, the quotes a browser generates for `en` |
| `sub`, `sup` | `Text` | `style.vertical-align: sub` / `super`, `style.font-size: 0.83em` | mapped | Inline span inside `Text`: the run is shifted and measured and drawn at 0.83 of the paragraph size ([`inline-size-runs.md`](../../design/inline-size-runs.md)); the line box stays the paragraph's |
| `small` | `Text` | `style.font-size: 0.83em` | mapped | Inline span inside `Text`, measured and drawn at 0.83 of the paragraph size (as `sub`) |
| `br` | `Text` | - | mapped | Void (`TESSERA_IR_UNSUPPORTED` with children). Inside a `Text` it is a span whose text is a line feed, a forced break that survives `white-space: normal`; written as a node of its own it is an empty `Text` |
| `hr` | `View` | - | mapped | Void. The user-agent sheet draws a zero-height box with a dark top and light bottom 1px edge and a `0.5em` margin, as Chrome's inset rule |
| `pre` | `Text` | `style.white-space: pre`, `style.font-family: monospace` | mapped | The user-agent sheet makes it a block with a `1em` vertical margin. Newlines come from a string expression or a span's static text |
| `blockquote` | `View` | - | mapped | The user-agent sheet gives it `margin: 1em 40px` |
| `progress`, `meter`, `details`, `summary` | `View` (carrier for the typings) | - | mapped | Lower onto the library `ProgressBar`, `Meter` and `Details` (section 3.8); `summary` only as a child of `details` |

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
| `"checkbox"`, `"radio"`, `"range"` | the library `Checkbox`, `Radio`, `Range`, not a `TextInput` | Section 3.8 |
| anything else (`file`, `date`, `time`, `color`, `hidden`, `submit`, `button`, `reset`, ...) | none: `TESSERA_JSX_SUBSET_HTML_INPUT_TYPE` | `submit`/`button`: `<Button>` |

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

### 3.6 Inline spans (`span`, `b`, `strong`, `i`, `em`, `code`, `a`, `u`, `s`, `small`, `mark`, `kbd`, `sub`, `sup`, `abbr`, `cite`, `q`, `br` inside `Text`)

Inside `Text` these tags accept the `Text` attribute set: `className`, `style`, `role`, the `aria-*`
attributes, `onClick` and `key`. `id` is accepted as common metadata. `ref`, `tabIndex`,
`autoFocus` and any event other than `onClick` are rejected on a span
(`TESSERA_JSX_SPAN_ATTRIBUTE`, suggestion: put them on the parent `Text`): a span is not a box and
has no focus order. No other attribute is rewritten; the `a` rewrites of section 3.5 still apply.
The span rules (static text, non-metric styles, hit testing, accessibility, limits) are
[`elements-basic.md`](elements-basic.md) section 2.1.

The same tags written outside `Text` remain plain `Text` elements (the emphasis defaults still
apply); only a `Text` child becomes an inline span.

### 3.7 `ol` and `li` numbering

`ol start={n}`, `ol reversed` and `li value={n}` rename to the `aria-start`, `aria-reversed` and
`aria-value` pairs the marker painter reads. The value is static: a bare flag, a string, or an integer
literal (`-2` included); `reversed` also takes `{true}`/`{false}`. Anything else is
`TESSERA_IR_UNSUPPORTED`. A dynamic value would need a metadata slot per item and is not in V1. The
counting rules are in [`list-style.md`](../../design/list-style.md) section 2.

### 3.8 Tags that lower onto library components (issue #1041, ADR-059 group E)

`input type="checkbox"|"radio"|"range"`, `progress`, `meter` and `details` resolve to a component of the library instead of an intrinsic ([`library_lowering.cpp`](../../../src/schema/src/library_lowering.cpp), `schema::find_library_lowering`). The file **imports the component under its library name** (`import { Checkbox } from "tessera-ui/Checkbox"`); a file without the import is `TESSERA_IR_UNRESOLVED` and the suggestion is the import line. The import is also how a project picks the styled component or its own. The `type` literal is dropped; every attribute below is rewritten onto a prop, and any other attribute (except `className`, `key`, `ref`, `automationId`) is `TESSERA_JSX_SUBSET_HTML_ATTR`.

| Tag | Component | Rewrites | Rejected |
|---|---|---|---|
| `input type="checkbox"` | `Checkbox` | `checked`, `disabled`; `onChange` to `onToggle`; `aria-label` to `label` | `defaultChecked` (the control is controlled), `name`, `required`, `form`, `value`, `autoFocus` |
| `input type="radio"` | `Radio` | `checked` to `selected`, `disabled`; `onChange` to `onSelect`; `aria-label` to `label` | as `checkbox` |
| `input type="range"` | `Range` | `defaultValue`, `min`, `max`, `step`; `aria-label` to `label` | `value` and `onChange` (a controlled range, #1051), `disabled`, the form attributes |
| `progress` | `ProgressBar` | `value`, `max`; `aria-label` to `label` | anything else |
| `meter` | `Meter` | `value`, `min`, `max`, `low`, `high`, `optimum`; `aria-label` to `label` | anything else |
| `details` | `Details` | `open` to `defaultOpen`; the first `<summary>` child is the `summary` prop | `name`, `onToggle` |

Values are integers (the primitives are `int`): `progress` and `meter` have no fractional value, and a `progress` without `max` has `max=100`, not 1 as in a browser. A callback attribute takes no parameter (`onChange={() => setOn(!on)}`), as every component callback does. The `<summary>` of a `details` holds static text; a `summary` anywhere else, a `details` without one, or a summary with elements is `TESSERA_IR_UNSUPPORTED`. The styled components are in [`component-library.md`](../../design/component-library.md); the typings carry the attributes of every row (`checked?`, `min?`, `open?`, ...).

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
| `form` | **Planned** (#544). Until then `TextInput` `onSubmit` plus `Button` `onClick`; the library `Form` and `Field` |
| `select`, `option`, `optgroup`, `datalist` | **Planned** (#544). Until then library `Select` |
| `table`, `thead`, `tbody`, `tfoot`, `tr`, `td`, `th`, `caption`, `colgroup`, `col` | **Planned**, implemented completely (#543, ADR-059 owner answer 3). Until then `View` with flex or grid style; `VirtualGrid` or library `DataGrid` for large data |
| `svg`, `g`, `circle`, `rect`, `line`, `polyline`, `polygon`, `ellipse`, `text`, `defs`, `use`, `path` | `Path` with `d` and `viewBox` ([Q17] for an automatic single-path rewrite) |
| `video`, `audio`, `picture`, `source`, `track`, `canvas` | **Planned** (#545). Until then `CustomElement` ([`custom-elements-v1.md`](../custom-elements-v1.md)) |
| `dialog`, `menu`, `popover` | **Planned** for `dialog` (#1052). Until then the overlay service and library components ([`overlays-and-layers-v1.md`](../overlays-and-layers-v1.md)) |
| `html`, `head`, `body`, `title`, `meta`, `link`, `style`, `base`, `noscript`, `template`, `slot` | None: the app shell, `tessera.json` and `import "./file.css"` replace them |
| `iframe`, `embed`, `object`, `script` | **Will not be done** (ADR-059 owner answer 5): they need a web engine or a JS runtime (ADR-024, ADR-036 item 6). Permanent `TESSERA_JSX_SUBSET_HTML_TAG` with the reason; the `dangerouslySetInnerHTML` prop is on the same list and keeps its own `TESSERA_JSX_SUBSET_DOM` |

## 6. Diagnostics

| Code | When | Severity |
|---|---|---|
| `TESSERA_JSX_SUBSET_HTML_TAG` | A lowercase tag with no mapping (section 5 or unknown). The code is kept for this case only | error |
| `TESSERA_JSX_SUBSET_HTML_ATTR` | An attribute with no V1 meaning on an alias or any intrinsic (new code) | error |
| `TESSERA_JSX_SUBSET_HTML_INPUT_TYPE` | `input type` outside section 3.2 (new code) | error |
| `TESSERA_JSX_PROP_CONFLICT` | `img width` with `style.width` and similar | error |

Both new codes carry a suggestion text with one text edit where the fix is mechanical (`class` to
`className`, `onMouseDown` to `onPointerDown`).
