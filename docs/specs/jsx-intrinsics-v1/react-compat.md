# React compatibility, code slice and open questions

Part of [`jsx-intrinsics-v1`](README.md). Owner direction: accept as much real-world React as is
safe, so existing projects port easily; typed state such as `useState<int>` is accepted as unavoidable.
Rule used throughout: prefer the React spelling when its meaning can be kept exactly; otherwise reject
with a diagnostic that names the Tessera form. Compatibility means source compatibility of the accepted
subset; it is not browser behavior ([ADR-036](../../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md) item 2).

## 1. Summary

| Area | Result |
|---|---|
| Lowercase tags | Accepted as aliases (decided, [ADR-039](../../../decisions/ADR-039-lowercase-html-tags-accepted.md), [`html-aliases.md`](html-aliases.md)) |
| `onClick`, `onChange`, `onFocus`, `onBlur`, `onKeyDown`, `onKeyUp`, `onSubmit` (inputs) | Same names. `onChange` is `(e) => setX(e.target.value)` as in React, and also `onChange={setX}` (Q1) |
| `className`, `style` (object, numbers are px for lengths), `key`, `ref` (from `useRef`), `disabled`, `readOnly`, `placeholder`, `value`, `defaultValue`, `maxLength`, `rows` | Same names and meaning |
| Fragments, `&&`, `?:`, `.map` | Accepted in the forms of subset 10.4/10.5 |
| `useMemo`, `useCallback` | Safe subset can be lowered as plain locals (section 4.2); no change now |
| Not accepted | Listed in section 4 with the reason |

## 2. Event props and payloads (decision for the owner: Q1)

Options for the payload of `onChange` on `TextInput`/`TextArea` (the other events have only one
sensible shape):

| Option | Handler sees | React port | Tessera spec |
|---|---|---|---|
| A. Plain value (the literal reading of [`text-editing-v1.md`](../text-editing-v1.md) section 7, `onChange={setEmail}`) | `string` | `(e) => setX(e.target.value)` fails to type-check; every handler must be rewritten | consistent |
| B. React synthetic event only (`e.target.value`) | `ChangeEvent` | ports unchanged | `onChange={setEmail}` of the text-editing example stops working |
| **C. Both: a record `TextChangeEvent { value, target: { value } }`; a setter reference receives the primary member `value`** | record | ports unchanged; also `e.value` | `onChange={setEmail}` keeps working; needs the rule of section 3 |

**Recommendation: C.** It is the only option that keeps both the existing spec example and the
dominant React idiom, and it costs one rule (primary member of a setter reference). Migration impact:
none for React code of the form `e => setX(e.target.value)`; React code that reads `e.target.checked`,
`e.currentTarget`, `e.nativeEvent`, `e.persist()` is not supported (no such members).

Synthetic event objects versus typed records: Tessera passes a typed record, never an object with
methods. `e.preventDefault()` and `e.stopPropagation()` are therefore not callable in TSX; they exist in
the developer C++ of native handlers only (Q3). The practical effect on ports: `<form onSubmit={e =>
{ e.preventDefault(); ... }}>` has no equivalent (no `form`, `html-aliases.md` section 5); the common
`onKeyDown` check `if (e.key === "Enter")` is also not expressible because handlers have no `if` (subset
9.2). The accepted workaround is `onSubmit` of `TextInput`, or a native handler.

Other event props: `onClick` (pointer, keyboard activation, assistive activate), `onFocus`, `onBlur`,
`onKeyDown`, `onKeyUp` keep their React names and meaning on the elements that table them
([`common-and-events.md`](common-and-events.md) section 4). `onMouse*` are not aliased to the pointer
events because a mouse event fires only for a mouse and a pointer event also for touch and pen: the
meaning would silently change (Q21). The diagnostic carries the one-token fix `onMouseDown` to
`onPointerDown`. `onSubmit` on `form` is unsupported; on `TextInput` it is Enter.

## 3. Proposed change to the subset spec: event payload rules (not applied)

Applies to [`jsx-subset-v1.md`](../jsx-subset-v1.md) only if Q1 is accepted as recommended. Not applied
in this round because the owner decision is open.

Section 9.1, current line (386):

```text
The name of a state setter: `onChange={setEmail}`. The event payload is passed to the setter and its
type must match the state type.
```

proposed:

```text
The name of a state setter: `onChange={setEmail}`. The setter receives the event's primary value (the
`value` of a text event, otherwise the whole payload) and its type must match the state type
(`jsx-intrinsics-v1/common-and-events.md` section 3, as a link in the real edit).
```

Section 9.4, add at the end: "The payload records, and which member is primary, are defined in
`jsx-intrinsics-v1/common-and-events.md` section 2." No other line changes.

## 4. React feature by feature

### 4.1 Accepted, supported and not supported

| React feature | Status | Why / migration |
|---|---|---|
| `children` of intrinsics (text, elements, expressions) | Supported; text under `View` becomes `Text` nodes ([`elements-basic.md`](elements-basic.md) section 0) | decision 11 |
| `children` prop of a user component (`{children}`) | Not supported [Q14] | `Component` nodes have no children in V1 ([`ir-v1.md`](../ir-v1.md) section 3.4) although [`component-model-v1.md`](../component-model-v1.md) section 3 says slots lower to child nodes. Pass elements through a typed prop once slots exist |
| `class` | Not accepted (`HTML_ATTR`) | React itself requires `className`; ports already use it |
| `htmlFor` | Not accepted [Q11] | No label to control association is specified. Use `aria-label` on the field, or the library `Field label` |
| `style={{ width: 100, flex: 1, opacity: 0.5 }}` | Supported | Numbers: px for length properties, unitless otherwise, from the stylesheet table (subset 10.3); `lineHeight: 1.5` stays unitless |
| `style={styles.box}`, `style={{...a, ...b}}` | Not supported | Keys must be known at compile time (`STYLE_DYNAMIC`). Inline the literal or use `className` |
| `disabled`, `readOnly`, `placeholder`, `value`, `defaultValue`, `maxLength`, `rows` | Supported | Same names; rules in [`elements-basic.md`](elements-basic.md) section 4 |
| `checked`, `defaultChecked` | Not supported | There is no checkbox intrinsic. Library `Checkbox`/`Switch` after V1 |
| `input type="text"`, `password`, `email`, `tel`, `url`, `search`, `number` | Supported through `input` ([`html-aliases.md`](html-aliases.md) section 3.2) | `number` keeps a string value |
| `input type="checkbox"`, `radio`, `range`, `file`, `date`, ... | Not supported (`HTML_INPUT_TYPE`) | No equivalent intrinsic |
| `value` without `onChange` | Warning, like React [Q4] | |
| `value` and `defaultValue` together | Error (React warns at run time) | One owner of the state |
| `e.target.value`, `e.currentTarget`, `e.target.checked`, `e.preventDefault()` | `e.target.value` supported on text events; others not | Section 2 |
| `parseInt(e.target.value)`, `Number(...)`, `.trim()`, any method call on a string | Not supported [Q26] | The pure-function allowlist has no string functions ([`ir-v1.md`](../ir-v1.md) section 4.2) |
| `key` on list roots | Supported, string or int | `key={index}` is accepted |
| `key` outside lists to force a remount | Not supported | Use `resetKey` of `ErrorBoundary`, or a state change |
| `ref={useRef<HTMLInputElement>(null)}` | Supported (alias type names) | Members of a ref are not defined in V1 [Q6]; `ref.current.focus()` is not expressible |
| Callback refs, `forwardRef`, `useImperativeHandle` | Not supported | Function values (subset 9) |
| `<>...</>` as a child | Supported (spliced) | |
| `<>...</>` as the returned root, `<Fragment key>` in a list | Not supported [Q18] | A component lowers to one root node; wrap in `View` |
| `<React.Fragment>` | Not supported | Member tags are rejected (subset 10.1); use `<>` |
| `cond && <X />`, `cond ? <A /> : <B />`, `cond ? <A /> : null` | Supported | `cond` must be `bool`: `{name && <X />}` and `{list.length && <X />}` are type errors (no truthiness). Write `name !== ""`, `length(list) > 0` |
| `{cond && "text"}` | Not supported | `&&` takes booleans; use `cond ? "text" : ""` |
| `.map` with a single expression body and `key` | Supported | Block bodies, `.filter().map()`, `.slice()`, `.sort()` are not (subset 10.5) |
| Large lists with `.map` | Works but use `VirtualList` | [`elements-other.md`](elements-other.md) section 3 |
| `onDoubleClick`, `onContextMenu`, `onWheel`, `onDrag*`, `onTouch*`, `onInput`, `onKeyPress`, `onMouse*` | Not supported [Q21] | No event of that name is specified |
| `tabIndex`, `autoFocus`, `title`, `data-*`, `hidden`, `dangerouslySetInnerHTML` | Not supported | No V1 meaning ([Q13] for focus) |
| `import { useState } from "react"` | Not supported today [Q15] | Only `"tessera"` is a bare module (subset 4) |
| Arrow-function components, `export default`, `memo(...)`, `lazy`, `Suspense`, context, portals, custom hooks, class components, `async` | Not supported | Subset 4, 6, 12 |
| `let`, `if`, `for`, `switch`, `try`, `??`, `?.` in component code | Not supported | Subset 6, 12 |

### 4.2 `useMemo` and `useCallback` (note only, no change)

Today both give `TESSERA_REACT_UNSUPPORTED_API`. A safe lowering exists for the common forms:

| Source | Safe lowering | Condition |
|---|---|---|
| `const v = useMemo(() => <PureExpr>, [deps])` | `const v = <PureExpr>` (a `LocalDecl`, inlined, decision 8) | arrow with an expression body that is a pure expression; the dependency list is ignored, or checked for exactness like `useEffect`. The memo's only effect in React is skipping recomputation, which a pure inlined expression does not need |
| `const f = useCallback(() => setN(n + 1), [n])` | `const f = () => setN(n + 1)`, a reusable inline handler (decision 4) | the arrow body is made of setter calls only (subset 9.2) |
| `// Note: bind::x` + `const f = useCallback((e: TextChangeEvent): void => {...}, [])` | the native handler form (subset 9.3) | the annotation sits before the `const`; deps ignored |
| `useReducer`, `useContext`, `useLayoutEffect`, `useId`, `useTransition`, `useDeferredValue`, `memo`, `forwardRef` | not safe | They need switch statements, context, different timing or ids |

If the owner adopts this, `TESSERA_REACT_UNSUPPORTED_API` stays for the unsafe rows and the first three
become accepted with an information-level note. This changes the hook rules of subset 7 and
[`component-model-v1.md`](../component-model-v1.md) section 12 (open question), so it is recorded as
Q16 and not applied.

## 5. Code slice required by ADR-039 (not done in this round)

The parser and tests currently reject lowercase tags. Nothing in code is changed by this documentation
round. The follow-up slice changes exactly:

| File | Function or test | Change |
|---|---|---|
| `src/compiler/analyzer/src/parser_jsx.cpp` | `Parser::check_tag_name` | Remove the lowercase branch that reports `TESSERA_JSX_SUBSET_HTML_TAG`; keep the `DYNAMIC_TAG` branch for `.` and `:` |
| same | `suggest_intrinsic` (anonymous namespace) | Delete; the suggestion comes from the alias table in analysis. The generic "tags start with an uppercase letter" text moves with it |
| same | `Parser::parse_named_element` (the call to `check_tag_name`) | Unchanged call, new behavior of the callee |
| `src/compiler/analyzer/src/parser_internal.hpp` | declaration of `check_tag_name` | Signature stays |
| `src/compiler/analyzer/include/tessera/compiler/jsx/parser.hpp` | comment near line 20 that lists "a lowercase tag" among parse-time subset rejections | Update the comment |
| `src/compiler/rules/` | new function in the analysis pass | Resolve alias tag, injected props and attribute rewrites from the table; report `HTML_TAG`, `HTML_ATTR`, `HTML_INPUT_TYPE`; record the canonical tag in the per-component analysis |
| `src/schema/` (new, [Q22]) | alias table | Source of the mapping |
| `tests/unit/jsx_parser_tests.cpp` | `test_subset_rejections`: `parse_jsx_element("<div>x</div>")`, the `<foo/>` suggestion check, `<div><span/></div>` | These parse without `HTML_TAG` in the parser; move the expectations to a `rules_*` test. `<foo/>` and others with no mapping are tested at the analysis level |
| same | `test_locations_with_crlf_and_non_ascii` (`<b/>` expects `HTML_TAG` at line 2, column 7) | Move the span check to the analysis test (`b` is unsupported there), or use a different parse-time error for the span check |
| same | the fixture at about line 220 that ends with `<div/>` | Check its expected error count |
| `tests/unit/jsx_concurrent_tests.cpp` | the case `<View a="1" b={x == y}> text &foo; <div/> </View>` | The expected diagnostics lose `HTML_TAG` at parse time; adjust the expected text |
| `tests/unit/` | new `rules_html_alias_tests.cpp` | One fixture per row of [`html-aliases.md`](html-aliases.md) sections 2, 3 and 5, including attribute rewrites and mutation checks |

Documentation edits already applied with this change: [`jsx-subset-v1.md`](../jsx-subset-v1.md)
sections 10.1, 11 (View children), 12 (the `Lowercase tags` row) and 18 (one sentence), plus one clause
of [`jsx-frontend.md`](../../design/jsx-frontend.md) and [`jsx-ir-lowering.md`](../../design/jsx-ir-lowering.md).
`jsx-subset-v1.md` is 776 lines, over the 600-line hard limit; it should be split into a folder when next
edited ([`docs/README.md`](../../README.md) rules). Net growth of this change to it: zero lines.

## 6. Open questions

Each has options and a recommendation. The tables of this folder are written as if the recommendation
holds, and the rows depend on the number in brackets.

| Q | Question | Options | Recommendation |
|---|---|---|---|
| 1 | `onChange` payload of text fields | A plain `string`; B React event only; C record with `value` and `target.value`, setter reference gets `value` | C (section 2) |
| 2 | Per-event payload field lists | Use the proposal of `common-and-events.md` section 2; or wait for a per-event table in `input.md` | Use the proposal now, move it to `input.md` when that spec lists fields |
| 3 | `preventDefault`/`stopPropagation` in TSX | Native handlers only; or a call form allowed in inline handlers | Native-only in V1; revisit with the first real port that needs it |
| 4 | `value` without `onChange` | Error; warning (like React); allow silently with the controlled-field behavior of `text-editing-v1.md` | Warning; and state in `text-editing-v1.md` that such a field behaves as read-only |
| 5 | Trigger of `TextArea` `onSubmit` | Not present on `TextArea`; Ctrl/Cmd+Enter; Enter | Ctrl/Cmd+Enter (Enter is a line break) |
| 6 | Members of a ref and which elements take `ref` | None in V1, refs only for developer C++; or a fixed method set (`focus`, `scrollTo`) callable in inline handlers | None in V1; the handler grammar has no method calls anyway |
| 7 | Vocabulary of `id`, `role`, `aria-*` | ARIA tokens (React-compatible) with the mapping of `common-and-events.md` 1.1; or Tessera role names | ARIA tokens; closed `aria-*` set until an id-relation spec exists; `id` is a selector target only |
| 8 | Element-valued attributes and `onError` form | Allow `{<X/>}` only for `ErrorBoundary.fallback`; or add a general slot rule; `onError` native or setter only | Allow only for `fallback`; correct the examples of `error-boundaries-v1.md` |
| 9 | JSX and IR shape of `VirtualList`/`VirtualGrid`; sticky headers | `.map` child plus an `itemTemplate` node field (needs an `ir-v1` addition); or reuse `loop` plus `children[0]`; header marking undefined | `.map` child and `itemTemplate`; defer `stickyHeaders` |
| 10 | `Scroll` props | `axis` only; also snap/physics props; `overflow` on `View` creates a scroll container | `axis` only; no overflow-created scrolling |
| 11 | `htmlFor`, `a href` | Reject (as written); define a label/control relation and a link target | Reject for V1; define with navigation and forms specs. Cost: label click and in-app links are lost until then |
| 12 | Default styles for aliases (heading size, bold) | None (as written); inline injected style; an opt-in shipped stylesheet | Opt-in `tessera-html-defaults.css` (never injected inline: inline would beat project class rules) |
| 13 | Focusable `View`, `tabIndex`, key/focus events on `View`; role inference for clickable `View` | Keep deferred and warn (as written); allow `tabIndex={0}` | Keep deferred; revisit with the accessibility spec |
| 14 | `children` prop of user components | Reject in V1 (as written); lower to child nodes as `component-model-v1.md` says, which needs the `Component` node to carry children | Decide before slice 9; the two documents conflict today |
| 15 | `import ... from "react"` | Reject; accept `useState`, `useEffect`, `useRef` from `"react"` as `"tessera"` and ignore `import React` | Accept the three hooks from `"react"`; every React file starts with this import |
| 16 | `useMemo`/`useCallback` | Keep rejecting; lower the safe forms of section 4.2 | Lower the safe forms; reject the rest |
| 17 | `svg` with one `path` | Unsupported (as written); rewrite to `Path` | Rewrite when `d` and `viewBox` are literals (typical ported icon) |
| 18 | Fragment as returned root | Reject (as written); wrap in an implicit `View` | Reject; an implicit box changes layout silently |
| 19 | Defaults and vocabularies: `rows`, `wrap`, `autofill`, `inputMode` extras | `rows` 2 as HTML; `wrap` `word`/`none`; `autofill` tokens of the alias table | As written; owner of `text-editing-v1.md` confirms |
| 20 | Is `Button label` and `Text` text a translation key? | Plain string; catalog key | Plain string; extraction is an i18n tool concern ([`i18n-v1.md`](../i18n-v1.md)) |
| 21 | Gesture and React-only event names (`onMouse*`, `onDoubleClick`, `onLongPress`, ...) | Reject with suggestions (as written); alias `onMouse*` to pointer events; define gesture handler props | Reject now; define gesture props with the gesture spec |
| 22 | Machine-readable form | A JSON embedded; B JSON plus generator; C `constexpr` C++ table plus dump tool | C, in a new leaf library `tessera_schema` ([`machine-readable.md`](machine-readable.md)) |
| 23 | Inconsistent examples in older specs | Fix: `custom-elements-v1.md` section 4 (`??`, `number | null`), `component-model-v1.md` section 1 (`number`, `useState(initial)` without a type argument, `<Text ref>` before ref members exist), `error-boundaries-v1.md` (`onError={(e) => report(e)}`) | Fix in a separate docs commit; not edited here |
| 24 | `secure` on `TextArea` | Reject (as written); allow | Reject |
| 25 | Icon-only `Button` without a name | Warning (as written); error | Warning, because the accessibility spec makes missing names a test failure anyway |
| 26 | String-to-number conversion in the pure allowlist | None (as written); add total functions such as `parseIntOr(s, fallback)` and `parseFloatOr(s, fallback)` | Add them (changes `ir-v1.md` section 4.2, evaluator and codegen together); numeric inputs are common in ports |
