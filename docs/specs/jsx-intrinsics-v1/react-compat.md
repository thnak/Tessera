# React compatibility, code slice and open questions

Part of [`jsx-intrinsics-v1`](README.md). Owner direction: accept as much real-world React as is
safe, so existing projects port easily; typed state such as `useState<int>` is accepted as unavoidable.
Rule used throughout: prefer the React spelling when its meaning can be kept exactly; otherwise reject
with a diagnostic that names the Tessera form. Compatibility means source compatibility of the accepted
subset; it is not browser behavior ([ADR-036](../../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md) item 2).

## 1. Summary

| Area | Result |
|---|---|
| Lowercase tags | Accepted as aliases (implemented, issue #592; [ADR-039](../../../decisions/ADR-039-lowercase-html-tags-accepted.md), [`html-aliases.md`](html-aliases.md)) |
| `onClick`, `onChange`, `onFocus`, `onBlur`, `onKeyDown`, `onKeyUp`, `onSubmit` (inputs) | Same names. `onChange` is `(e) => setX(e.target.value)` as in React, and also `onChange={setX}` (Q1) |
| `className`, `style` (object, numbers are px for lengths), `key`, `ref` (from `useRef`), `disabled`, `readOnly`, `placeholder`, `value`, `defaultValue`, `maxLength`, `rows` | Same names and meaning |
| Fragments, `&&`, `?:`, `.map` | Accepted in the forms of subset 10.4/10.5 |
| `useMemo`, `useCallback` | Safe subset can be lowered as plain locals (section 4.2); no change now |
| Not accepted | Listed in section 4 with the reason |

### 1.1 Port classes (ADR-058), and the checker that reports them

A port of an existing React project is graded per dependency by how much of it survives
([ADR-058](../../../decisions/ADR-058-react-vite-port-compatibility-tiers.md)). The classes are
the vocabulary of that grade, and `tessera port-check <dir>` (issue #546) prints them:

| Class | Meaning | Examples |
|---|---|---|
| A | Source-compatible: the same source compiles | `react` (an alias of `tessera`, section 4.1) |
| B | Shim: a Tessera module provides the API under the same import | `react-router-dom` |
| C | Build-time: the build rewrites it | `vite`, `react-dom`, `*.module.css` |
| D | Not ported: no equivalent, and the row names what to write instead | `@mui/material`, `styled-components`, `@tanstack/react-query` |

Two rules make the report usable rather than decorative. **A class-D dependency is never a bare
refusal**: ADR-058 section 1 forbids a silent drop, so every D row names its replacement, and a
test walks the table to keep that true. **An unclassified package is class D**, not an optimistic
guess (ADR-058 section 4), so a dependency nobody has ruled on shows up as a blocker rather than
disappearing from the totals.

The checker reads only `package.json`: it never reads `node_modules` (ADR-058 section 4), costs no
build, and therefore runs on a fresh checkout. Construct-level grading - which `localStorage` or
`dangerouslySetInnerHTML` a file uses - belongs to the front end and arrives with typing rules 1-3
of ADR-058 section 3, which are steps 2 and later of its order of work.

### 1.2 One-line-fix typing diagnostics (ADR-058 typing rule 3, #548)

The edits a port is asked to make by hand each have a stable code, a location and the fix in the message, so `port-check` can list them verbatim:

| Written | Code | Fix |
|---|---|---|
| `any`, `unknown` | `TESSERA_JSX_TYPE_ANY` | the concrete type, or `type Item = { ... }` |
| `Record<K, V>`, `Map<K, V>` | `TESSERA_JSX_TYPE_MAP` | `Entry[]` with `type Entry = { key: string; value: T }` until a map type exists |
| `useState([])`, `useState({})` | `TESSERA_JSX_TYPE_STATE_UNTYPED` | `useState<Item[]>([])`, `useState<Item>({})` |

Absorbed without an edit (typing rule 1, #547 slice 1): `interface X { ... }` and `export interface X { ... }` are the type alias `type X = { ... }`, and `import type` / `export type { }` were already accepted. `interface X extends Y` and a generic interface stay `TESSERA_JSX_SUBSET_TYPES` with the fix to write the members out. Slice 2: `Partial<R>`, `Required<R>`, `Pick<R, "a" | "b">`, `Omit<R, "a">` and `A & B` over records declared in the same file are expanded at module level to the one object type of the resulting members (so a later alias may be used before its declaration, and they compose). Codes: `TESSERA_JSX_TYPE_UTILITY` (a record not declared in this file, wrong arity, non-literal or unknown keys) and `TESSERA_JSX_TYPE_INTERSECTION` (a non-record operand, or a member declared twice). Slice 3: a string `enum` (`enum Color { Red = "red" }`, also exported) is `type Color = "red" | ...`, and `Color.Red` is replaced by the literal at parse time; an `as const` suffix on a constant is accepted and dropped, and `const COLORS = { dark: "d" } as const` makes `COLORS.dark` the literal `"d"`. A numeric or valueless enum member, or an unknown member, is `TESSERA_JSX_TYPE_ENUM` with the fix. `typeof COLORS[keyof typeof COLORS]` after the declaration is the union of its string values; any other `typeof` type is `TESSERA_JSX_SUBSET_TYPES`. The namespace must be declared earlier in the same file; that is the remaining gap (#547 follow-up).

An inline object type on the props parameter stays accepted: the lowering already gives it a synthesized record, so no diagnostic is raised for it.

## 2. Event props and payloads (decided: Q1, option C)

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

## 3. Change to the subset spec: event payload rules (Q1, decided and applied)

Q1 was accepted as recommended. The edit below is applied to
[`jsx-subset-v1.md`](../jsx-subset-v1.md) sections 9.1 and 9.4; the text is kept here as the record.

Section 9.1, previous text:

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
| `children` prop of a user component (`{children}`) | Supported as a static slot [Q14, O2] | Normative in [`ir-v1.md`](../ir-v1.md) section 3.4 (`Component` children and `Slot`) and subset section 5 (`Children` type); the front end rejects it until the code slice lands |
| `class` | Not accepted (`HTML_ATTR`) | React itself requires `className`; ports already use it |
| `htmlFor` | Not accepted [Q11] | No label to control association is specified. Use `aria-label` on the field, or the library `Field label` |
| `style={{ width: 100, flex: 1, opacity: 0.5 }}` | Supported | Numbers: px for length properties, unitless otherwise, from the stylesheet table (subset 10.3); `lineHeight: 1.5` stays unitless |
| `style={styles.box}`, `style={{...a, ...b}}` | Not supported | Keys must be known at compile time (`STYLE_DYNAMIC`). Inline the literal or use `className` |
| `disabled`, `readOnly`, `placeholder`, `value`, `defaultValue`, `maxLength`, `rows` | Supported | Same names; rules in [`elements-basic.md`](elements-basic.md) section 4 |
| `checked` | `input type="checkbox"`/`"radio"` lower onto the library `Checkbox`/`Radio` ([`html-aliases.md`](html-aliases.md) section 3.8); `checked` on a text input is `HTML_ATTR` | The control is controlled; `defaultChecked` is rejected |
| `input type="text"`, `password`, `email`, `tel`, `url`, `search`, `number` | Supported through `input` ([`html-aliases.md`](html-aliases.md) section 3.2) | `number` keeps a string value |
| `input type="checkbox"`, `radio`, `range` | Mapped to library `Checkbox`, `Radio`, `Range` (section 3.8) | The file imports the component |
| `input type="file"`, `date`, ... | Not supported (`HTML_INPUT_TYPE`) | No equivalent intrinsic |
| `value` without `onChange` | Warning, like React [Q4] | |
| `value` and `defaultValue` together | Error (React warns at run time) | One owner of the state |
| `e.target.value`, `e.currentTarget`, `e.target.checked`, `e.preventDefault()` | `e.target.value` supported on text events; others not | Section 2 |
| `parseInt(e.target.value)`, `Number(...)`, `.trim()`, any method call on a string | Not supported as written; `parseIntOr(s, fallback)` and `parseFloatOr(s, fallback)` are decided additions (Q26) | The allowlist has no string functions today ([`ir-v1.md`](../ir-v1.md) section 4.2); the addition is pending in that spec, the evaluator and codegen |
| `key` on list roots | Supported, string or int | `key={index}` is accepted |
| `key` outside lists to force a remount | Not supported | Use `resetKey` of `ErrorBoundary`, or a state change |
| `ref={useRef<HTMLInputElement>(null)}` | Supported (alias type names) | A ref has no `.current`: `r.focus()` and `r.blur()` are the only members, written as handler statements ([common-and-events.md](common-and-events.md) section 4); other members stay undefined [Q6] |
| Callback refs, `forwardRef`, `useImperativeHandle` | Not supported | Function values (subset 9) |
| `<>...</>` as a child | Supported (spliced) | |
| `<>...</>` as the returned root, `<Fragment key>` in a list | Not supported [Q18] | A component lowers to one root node; wrap in `View` |
| `<React.Fragment>` | Not supported | Member tags are rejected (subset 10.1); use `<>` |
| `cond && <X />`, `cond ? <A /> : <B />`, `cond ? <A /> : null` | Supported | `cond` must be `bool`: `{name && <X />}` and `{list.length && <X />}` are type errors (no truthiness). Write `name !== ""`, `length(list) > 0` |
| `{cond && "text"}` | Not supported | `&&` takes booleans; use `cond ? "text" : ""` |
| `.map` with a single expression body and `key` | Supported | Block bodies, `.filter().map()`, `.slice()`, `.sort()` are not (subset 10.5) |
| Large lists with `.map` | Works but use `VirtualList` | [`elements-other.md`](elements-other.md) section 3 |
| `onDoubleClick`, `onContextMenu`, `onWheel`, `onDrag*`, `onTouch*`, `onInput`, `onKeyPress`, `onMouse*` | Not supported [Q21] | No event of that name is specified |
| `tabIndex`, `autoFocus` | Supported on `View` and `Text` (issue #226) | `tabIndex` is `{0}` or `{-1}`; semantics required ([common-and-events.md](common-and-events.md) section 4); `autoFocus` on inputs stays `HTML_ATTR`. Evidence: `focusable_view` |
| `id`, `aria-labelledby`, `aria-describedby`, `aria-controls` | Supported, and `id` takes an expression since ADR-066 item 1 | The id-reference attributes are what `useId` pairs with: `const id = useId(); <View id={id} aria-labelledby={id} />`. `id` was a literal-only selector target before, which made the pair unwritable |
| `title`, `data-*`, `hidden`, `dangerouslySetInnerHTML` | Not supported | No V1 meaning |
| `import { useState, useEffect, useRef } from "react"` | Supported (Q15): alias of `"tessera"` | Any other name from `"react"` stays an error. `import React from "react"` and `import * as React from "react"` are accepted and ignored (ADR-066 item 1); a differently named default or namespace import stays an error (subset 4) |
| Arrow-function components, `export default function Name`, `memo(...)` | Supported (ADR-066 item 1) | An arrow component is an uppercase `const` bound to an arrow with the same parameter and body rules; `export default function Name` keeps its own name; `memo(Component)` is identity, because updates already follow dependencies |
| `StrictMode`, `Profiler` | Supported as pass-through (ADR-066 item 1) | There is no development double-render and no profiler, so the wrapper contributes nothing and its children are spliced where it stood |
| `useId`, `useLayoutEffect` | Supported (ADR-066 item 1) | `useId` is a stable string from the instance identity and the call ordinal; `useLayoutEffect` has `useEffect` timing, because there is no browser paint to order against |
| `lazy`, `Suspense`, context, portals, class components, `async` | Not supported | Subset 4, 6, 12; `Suspense` and `lazy` are ADR-067 |
| Custom hooks | Supported: expanded at each call site ([`custom-hooks-v1.md`](../custom-hooks-v1.md)) | ADR-065 |
| `let`, `if`, `for`, `switch`, `try` in component code | Not supported | Subset 6, 12; handler control flow is ADR-066 item 2 |
| `??`, `?.` over an `optional<T>` | Supported (ADR-066 item 1) | Both are IR expression kinds (`coalesce`, `optionalField`), because the IR has no null literal to compare against. A plain `.` on an optional stays an error |

### 4.2 `useMemo` and `useCallback` (decided: Q16, safe forms lower)

**Landed** (ADR-066 item 1). The safe forms below are accepted; the parser desugars them, so every
later pass sees the ordinary form it already handles. A form outside them keeps
`TESSERA_REACT_UNSUPPORTED_API`.

| Source | Safe lowering | Condition |
|---|---|---|
| `const v = useMemo(() => <PureExpr>, [deps])` | `const v = <PureExpr>` (a `LocalDecl`, inlined, decision 8) | arrow with an expression body that is a pure expression; the dependency list is **checked for agreement, not exactness** (section 4.2.1). The memo's only effect in React is skipping recomputation, which a pure inlined expression does not need |
| `const f = useCallback(() => setN(n + 1), [n])` | `const f = () => setN(n + 1)`, a reusable inline handler (decision 4) | the arrow body is made of setter calls only (subset 9.2) |
| `// Note: bind::x` + `const f = useCallback((e: TextChangeEvent): void => {...}, [])` | the native handler form (subset 9.3) | the annotation sits before the `const`; deps checked for agreement |
| `useReducer`, `useContext`, `useTransition`, `useDeferredValue`, `forwardRef` | not safe | They need switch statements, context or different timing. `useReducer` and `useContext` are ADR-066 items 4 and 5; `useTransition` and `useDeferredValue` are ADR-067 |

`TESSERA_REACT_UNSUPPORTED_API` stays for the unsafe rows and for any `useMemo`/`useCallback` outside
the first three rows. The subset spec section 7 table carries the two forms.

#### 4.2.1 The dependency list is compared, not obeyed (issue #829)

React is a runtime library, so its author has to declare the memo dependencies by hand and a wrong
list silently keeps a stale value. Tessera compiles statically and infers the reactive inputs, so
**the inferred set is authoritative** and the declared list only serves to tell the author where
their port disagrees with it. React Compiler infers them at build time too; the array is an escape
hatch for a compiler that cannot see a closure.

| Declared list vs inferred set | Behaviour | Diagnostic |
|---|---|---|
| Exact | as today | none |
| Missing a name the body reads | recompute on the inferred set (no stale value) | **warning** `TESSERA_REACT_MEMO_DEPS`: "`query` is read but not listed; Tessera recomputes when it changes, React would keep the old value", fix-it to add it |
| Lists a name the body does not read | ignored | warning, fix-it to remove it |
| Element is not a plain name | — | error, the same rule `useEffect` has |
| No array (`useMemo(() => x)`) | recompute | accepted, no diagnostic |

A mismatch is **never an error**: with React Compiler the array does not decide behaviour in React
either, so the compiler's set wins and the warning points at the difference. There is no strict or
React-parity mode. `useEffect` keeps its exact rule on purpose (section 4.2.2).

The inferred set is the reactive names the inlined body reads after local inlining, so a memo over
another memo or local resolves through to props, state variables and animated values.

#### 4.2.2 Why `useEffect` stays exact

For an effect the array is *intent*, not optimisation: `[]` means "run once at mount" even when the
body reads props, so inferring effect dependencies would change meaning. A memo's array only ever
skips a recomputation, so inferring it changes nothing the author can observe except a stale value
they did not want. The two hooks therefore keep different rules on purpose.

### 4.3 `children` of user components (Q14, decided: O2)

The two normative statements conflict:

| [`component-model-v1.md`](../component-model-v1.md) section 3 | [`ir-v1.md`](../ir-v1.md) section 3.4 (and subset section 5) |
|---|---|
| "`children`/slots are explicit props; the compiler lowers them to child nodes." | A `Component` node has `component`, `props`, `events`; "`children` is empty in V1". V1 types have `node_ref` but no type for node content, and subset section 5 has no `Node`/`ReactNode` type to annotate `children` with |

Effect on ports: `function Card({ children }: CardProps)` and `<Card>...</Card>` are the most common
React composition pattern, and the library `Card`, `Dialog`, `Field` need slots after V1.

Options:

| Option | What it means | Cost |
|---|---|---|
| O1. No component children in V1 | Follow `ir-v1.md`. `children` is not a prop; a child inside a user component tag is `TESSERA_JSX_CHILDREN_NOT_ALLOWED` with the suggestion to pass data props. Fix the sentence in `component-model-v1.md` section 3 | No IR change. Every `<Card>` wrapper in a port must be rewritten |
| O2. Static slot: `children` content is compiled into the callee | New type (for example `Children`) for a prop named `children`; `Component` node carries `children`; callee writes `{children}` as a child expression, lowered to a new IR `Slot` position or to inlined nodes per instance | New IR addition (closed kind list or a node field), validator, evaluator and runtime reconcile rules; instances keep separate state because the slot content is owned by the caller |
| O3. Named element props only | Reuse the element-valued attribute of Q8 for any component prop typed `Element` (`header={<X />}`); no implicit `children` | Same IR need as O2 for one subtree per prop; no implicit children, so `<Card>..</Card>` ports still need edits |

Decision (owner, 2026-11-09): **O2**, a static `Children` slot. Porting `<Card>...</Card>` unchanged
outweighs the IR cost. The owning specs now state it: `ir-v1.md` section 3.4
(`Component` children plus a `Slot` node, additive so `irVersion` stays `1`),
`component-model-v1.md` section 3 and `jsx-subset-v1.md` sections 5 and 10.4. The front end still
rejects component children until the code slice lands.

## 5. Code slice required by ADR-039 (first slice landed, issue #592)

The front end no longer rejects lowercase tags. Issue #592 (the first slice of the group C work of
[ADR-059](../../../decisions/ADR-059-html-tags-supported-not-rejected.md)) removed the parse-time
rejection and the alias resolution guard, so a mapped tag compiles and a tag with no mapping keeps
its own diagnostic. The parser holds no alias table: `tessera_schema` is the only source.

| File | Function or test | State |
|---|---|---|
| `src/compiler/analyzer/src/parser_jsx.cpp` | `Parser::check_tag_name` | **Done.** The lowercase branch that reported `TESSERA_JSX_SUBSET_HTML_TAG` is removed; the `DYNAMIC_TAG` branch for `.` and `:` stays |
| same | `suggest_intrinsic` (anonymous namespace) | **Done.** Deleted with the duplicate table; the suggestion now comes from the alias row (`AliasRow::alternative`), or the generic "use an intrinsic such as `<View>` or `<Text>`" for an unknown tag |
| same | `Parser::parse_named_element` (the call to `check_tag_name`) | Unchanged call, new behavior of the callee |
| `src/compiler/analyzer/include/tessera/compiler/jsx/parser.hpp` | comment near line 20 that listed "a lowercase tag" among parse-time subset rejections | **Done.** Updated |
| `src/compiler/lowering/src/schema_bridge.cpp` | `resolve_element` | **Done.** An alias resolves through `find_element(alias->target)` + `ir_node_kind` wherever it is written, not only inside a `Text` span; `alias->inject` and `alias_style_defaults` still apply. An `Unsupported` row and a tag in neither table report `TESSERA_JSX_SUBSET_HTML_TAG` here, since the parser no longer can |
| `src/compiler/rules/` | attribute rewrites from the table (`class` to `className`, `input type`, `autoComplete`, `img width`, `textarea wrap`) and `TESSERA_JSX_SUBSET_HTML_ATTR` / `HTML_INPUT_TYPE` | **Not done.** The data (`AliasRow::attrs`, `type_map`, `common_alias_attrs()`) exists; no pass reads it yet. This is the rest of slice 13 ([`html-aliases.md`](html-aliases.md) section 3) |
| `src/compiler/rules/` | `from "react"` alias and the `useMemo`/`useCallback` safe forms ([Q15], [Q16]) | **Not done** |
| `src/schema/` | alias table | Done earlier (slice 12); this slice adds no second table |
| typings | `JSX.IntrinsicElements` for the alias tags | **Not done** |

Test coverage of the slice:

| Test | Covers |
|---|---|
| `tests/unit/lowering_inline_span_tests.cpp` | One case per mapped tag of [`html-aliases.md`](html-aliases.md) section 2 (`div`, `span`, `p`, `h1`-`h3`, `section`, `main`, `label`, `button`, `img`, `input`, `textarea`): the carrier of the row, the AST keeping the tag as written, the injected role, and the closed IR kind. Also the `HTML_TAG` cases (an unknown tag and a section-5 row) and the inline-span rules |
| `tests/unit/jsx_parser_tests.cpp` | A lowercase tag parses (no parse-time `HTML_TAG`); member and namespace tags are still rejected; the CRLF/non-ASCII span check uses a member tag |
| `tests/unit/jsx_diagnostics_cases.hpp` | `TESSERA_JSX_SUBSET_HTML_TAG` is an explicit skip instead of a fixture: lowering owns it now, and this registry scans the front end and the rules only |
| new `rules_html_alias_tests.cpp` | **Not done**, waits for the attribute rewrites above: one fixture per row of `html-aliases.md` sections 2, 3 and 5, including attribute rewrites and mutation checks |

Documentation edits already applied: [`jsx-subset-v1.md`](../jsx-subset-v1.md) sections 4 (`"react"`
import, Q15), 9.1 and 9.4 (Q1), 10.1, 11, 12 (the `Lowercase tags` and `useMemo`/`useCallback` rows, Q16)
and 18; a clause of [`jsx-frontend.md`](../../design/jsx-frontend.md); and in
[`jsx-ir-lowering.md`](../../design/jsx-ir-lowering.md) the slice list (items 12 and 13), the hook-rule row
and the notes. `tessera_schema` is slice 12. `jsx-subset-v1.md` is about 780 lines, over the 600-line
hard limit; it should be split into a folder when next edited
([`docs/README.md`](../../README.md) rules).

## 6. Open questions

The owner accepted every recommendation below; Q14 was decided as O2 (section 4.3). The tables of
this folder follow the decided options; rows depend on the number in brackets. Decisions that need an
edit of a spec this folder does not own are pending there: `ir-v1.md` (Q26 allowlist functions; the Q9 `itemTemplate`
field is done, section 3.5), `text-editing-v1.md` (Q4 read-only behavior, Q5 trigger, Q19 defaults),
`error-boundaries-v1.md` and `custom-elements-v1.md` and `component-model-v1.md` (Q23 examples, Q8),
`accessibility-v1.md` (Q7 tokens), `input.md` (Q2 fields).

| Q | Status | Question | Options | Recommendation |
|---|---|---|---|---|
| 1 | Decided: C | `onChange` payload of text fields | A plain `string`; B React event only; C record with `value` and `target.value`, setter reference gets `value` | C (section 2) |
| 2 | Decided: use the proposal | Per-event payload field lists | Use the proposal of `common-and-events.md` section 2; or wait for a per-event table in `input.md` | Use the proposal now, move it to `input.md` when that spec lists fields |
| 3 | Decided: native-only | `preventDefault`/`stopPropagation` in TSX | Native handlers only; or a call form allowed in inline handlers | Native-only in V1; revisit with the first real port that needs it |
| 4 | Decided: warning | `value` without `onChange` | Error; warning (like React); allow silently with the controlled-field behavior of `text-editing-v1.md` | Warning; and state in `text-editing-v1.md` that such a field behaves as read-only |
| 5 | Decided: Ctrl/Cmd+Enter | Trigger of `TextArea` `onSubmit` | Not present on `TextArea`; Ctrl/Cmd+Enter; Enter | Ctrl/Cmd+Enter (Enter is a line break) |
| 6 | Decided: none in V1 | Members of a ref and which elements take `ref` | None in V1, refs only for developer C++; or a fixed method set (`focus`, `scrollTo`) callable in inline handlers | None in V1; the handler grammar has no method calls anyway |
| 7 | Decided: ARIA tokens, closed set | Vocabulary of `id`, `role`, `aria-*` | ARIA tokens (React-compatible) with the mapping of `common-and-events.md` 1.1; or Tessera role names | ARIA tokens; closed `aria-*` set until an id-relation spec exists; `id` is a selector target only |
| 8 | Decided: only `fallback` | Element-valued attributes and `onError` form | Allow `{<X/>}` only for `ErrorBoundary.fallback`; or add a general slot rule; `onError` native or setter only | Allow only for `fallback`; correct the examples of `error-boundaries-v1.md` |
| 9 | Decided: `.map` child + `itemTemplate`; `stickyHeaders` deferred | JSX and IR shape of `VirtualList`/`VirtualGrid`; sticky headers | `.map` child plus an `itemTemplate` node field (needs an `ir-v1` addition); or reuse `loop` plus `children[0]`; header marking undefined | `.map` child and `itemTemplate`; defer `stickyHeaders` |
| 10 | Decided: `axis` only | `Scroll` props | `axis` only; also snap/physics props; `overflow` on `View` creates a scroll container | `axis` only; no overflow-created scrolling |
| 11 | Decided: reject | `htmlFor`, `a href` | Reject (as written); define a label/control relation and a link target | Reject for V1; define with navigation and forms specs. Cost: label click and in-app links are lost until then |
| 12 | Superseded by [ADR-063](../../../decisions/ADR-063-browser-matching-user-agent-defaults.md) (2026-11-20) | Default styles for aliases (heading size, bold) | Was: an opt-in `tessera-html-defaults.css`. Now: part of the engine's user-agent sheet | Automatic, at the lowest cascade origin so any author rule wins; nothing injected inline. The browser look is the default; a difference needs a named reason |
| 13 | Closed by #226 (focusable `View`, `tabIndex`, key/focus events) | Focusable `View`, `tabIndex`, key/focus events on `View`; role inference for clickable `View` | Keep deferred and warn (as written); allow `tabIndex={0}` | Keep deferred; revisit with the accessibility spec |
| 14 | Decided: O2 (section 4.3) | `children` prop of user components | Reject in V1 (as written); lower to child nodes as `component-model-v1.md` says, which needs the `Component` node to carry children | Static `Children` slot; specs updated, code in slice 9 |
| 15 | Decided: accept the three hooks | `import ... from "react"` | Reject; accept `useState`, `useEffect`, `useRef` from `"react"` as `"tessera"` and ignore `import React` | Accept the three hooks from `"react"`; every React file starts with this import |
| 16 | Decided: lower safe forms; compare the list | `useMemo`/`useCallback` | Keep rejecting; lower the safe forms of section 4.2 | Lower the safe forms; reject the rest. Issue #829: the declared list is compared with the inferred set and a mismatch warns (`TESSERA_REACT_MEMO_DEPS`), never errors; an absent array is accepted (section 4.2.1) |
| 17 | Decided: rewrite | `svg` with one `path` | Unsupported (as written); rewrite to `Path` | Rewrite when `d` and `viewBox` are literals (typical ported icon) |
| 18 | Decided: reject | Fragment as returned root | Reject (as written); wrap in an implicit `View` | Reject; an implicit box changes layout silently |
| 19 | Decided: as written | Defaults and vocabularies: `rows`, `wrap`, `autofill`, `inputMode` extras | `rows` 2 as HTML; `wrap` `word`/`none`; `autofill` tokens of the alias table | As written; owner of `text-editing-v1.md` confirms |
| 20 | Decided: plain string | Is `Button label` and `Text` text a translation key? | Plain string; catalog key | Plain string; extraction is an i18n tool concern ([`i18n-v1.md`](../i18n-v1.md)) |
| 21 | Decided: reject now | Gesture and React-only event names (`onMouse*`, `onDoubleClick`, `onLongPress`, ...) | Reject with suggestions (as written); alias `onMouse*` to pointer events; define gesture handler props | Reject now; define gesture props with the gesture spec |
| 22 | Decided: C | Machine-readable form | A JSON embedded; B JSON plus generator; C `constexpr` C++ table plus dump tool | C, in a new leaf library `tessera_schema` ([`machine-readable.md`](machine-readable.md)) |
| 23 | Decided: fix in a docs commit | Inconsistent examples in older specs | Fix: `custom-elements-v1.md` section 4 (`??`, `number | null`), `component-model-v1.md` section 1 (`number`, `useState(initial)` without a type argument, `<Text ref>` before ref members exist), `error-boundaries-v1.md` (`onError={(e) => report(e)}`) | Fix in a separate docs commit; not edited here |
| 24 | Decided: reject | `secure` on `TextArea` | Reject (as written); allow | Reject |
| 25 | Decided: warning | Icon-only `Button` without a name | Warning (as written); error | Warning, because the accessibility spec makes missing names a test failure anyway |
| 26 | Decided: add `parseIntOr`/`parseFloatOr` | String-to-number conversion in the pure allowlist | None (as written); add total functions such as `parseIntOr(s, fallback)` and `parseFloatOr(s, fallback)` | Add them (changes `ir-v1.md` section 4.2, evaluator and codegen together); numeric inputs are common in ports |
