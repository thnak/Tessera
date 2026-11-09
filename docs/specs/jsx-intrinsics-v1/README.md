# JSX intrinsic element table v1

Status: proposed (Big Step C, defined in [AGENTS.md](../../../AGENTS.md#big-steps-group-work-dont-micro-step)).
Decision records: [ADR-036](../../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md)
(own JSX/TSX front end), [ADR-039](../../../decisions/ADR-039-lowercase-html-tags-accepted.md)
(lowercase HTML tags are aliases).

This folder is the single normative table of the built-in (intrinsic) elements of the JSX/TSX front
end: which children, props, events and refs each accepts, and how each lowers to IR. It fills the gap
noted in [`jsx-ir-lowering.md`](../../design/jsx-ir-lowering.md) section 11. The syntax is defined in
[`jsx-subset-v1.md`](../jsx-subset-v1.md) (the "subset spec"); node kinds in [`ir-v1.md`](../ir-v1.md);
semantics in the element specs named per element. This folder does not repeat those; where one of
them is silent or conflicts, the choice made here is the least committal one and is listed as a numbered
question in [`react-compat.md`](react-compat.md) section 6 (written `[Q n]` in the tables).

## 0. Parts

| File | Content |
|---|---|
| [`README.md`](README.md) | This page: terms, notation, lowering rules shared by all elements |
| [`common-and-events.md`](common-and-events.md) | Common and reserved attributes; event payload records; handler rules |
| [`elements-basic.md`](elements-basic.md) | `View`, `Text`, `Button`, `TextInput`, `TextArea` (including the owner's cases) |
| [`elements-other.md`](elements-other.md) | `Image`, `Path`, `Scroll`, `Spacer`, `VirtualList`, `VirtualGrid`, `ErrorBoundary`, `CustomElement` |
| [`html-aliases.md`](html-aliases.md) | Normative mapping of lowercase HTML tags and HTML attributes |
| [`react-compat.md`](react-compat.md) | React compatibility: what is accepted, what is not and why; code slice list; open questions |
| [`machine-readable.md`](machine-readable.md) | Machine-readable form, owner, consumers, sharing with the runtime and the typings generator |

## 1. Terms

| Term | Meaning |
|---|---|
| Intrinsic | A built-in element whose tag is one of the 13 names in section 3 |
| Alias | A lowercase HTML tag that resolves to an intrinsic ([`html-aliases.md`](html-aliases.md)) |
| Literal-only | The attribute value must be a string, number or boolean literal (or the value-less flag form for `bool`); an expression is `TESSERA_JSX_PROP_NOT_LITERAL` |
| Pure expression | As in subset spec section 8; a literal is also accepted |
| Handler | A function value of subset spec section 9 (setter reference, inline setter arrow, native handler) |
| Run | A maximal sequence of JSX text and string/number expression children with no element between them |
| Payload | The single argument an event passes to its handler; a record type of [`common-and-events.md`](common-and-events.md) section 2 |

## 2. Notation of the element tables

Prop table columns:

| Column | Meaning |
|---|---|
| Prop | Attribute name as written on the capitalized intrinsic |
| Type | A type of subset spec section 5 plus the record types of section 2 of `common-and-events.md`. `enum` lists string literals. `length` is a number of dp or a string with a unit ([`stylesheet-v1.md`](../stylesheet-v1.md) section 8.1) |
| Req | `yes`, `no`, or a constraint id (`one-of A`) defined under the table |
| Default | Value when absent; `-` means the prop is absent in IR |
| Value | `L` literal-only, `E` pure expression (literal allowed), `H` handler, `N` element-valued ([Q8]) |
| IR | Where it lowers: `props.x`, `style.x`, `text`, `key`, `refs`, `events.x`, `automationId`, `children` |

Diagnostics use these generic codes (new, not in subset spec section 12; they follow the naming of
`TESSERA_JSX_IMAGE_ALT`): `TESSERA_JSX_PROP_REQUIRED`, `TESSERA_JSX_PROP_CONFLICT`,
`TESSERA_JSX_PROP_ONE_OF`, `TESSERA_JSX_PROP_NOT_LITERAL`, `TESSERA_JSX_CHILDREN_NOT_ALLOWED`,
`TESSERA_JSX_CHILDREN_COUNT`, plus the element-specific codes named in the tables. An unknown
attribute or event name is `TESSERA_JSX_SUBSET_UNKNOWN_PROP` (subset spec 10.2) in every case.
Types of attribute values are checked by the type pass (`TESSERA_JSX_TYPE_MISMATCH`).

## 3. The intrinsic list

| Tag | IR kind | Children | Ref element type | Defined in |
|---|---|---|---|---|
| `View` | `View` | nodes and text runs | `View` | [`elements-basic.md`](elements-basic.md) |
| `Text` | `Text` | text runs | `Text` | same |
| `Button` | `Button` | text runs and nodes | `Button` | same |
| `TextInput` | `TextInput` | none | `TextInput` | same |
| `TextArea` | `TextArea` | none | `TextArea` | same |
| `Image` | `Image` | none | `Image` | [`elements-other.md`](elements-other.md) |
| `Path` | `Path` | none | `Path` | same |
| `Scroll` | `Scroll` | nodes | `Scroll` | same |
| `Spacer` | `Spacer` | none | none (`ref` rejected) | same |
| `VirtualList` | `VirtualList` | exactly one `.map` template | `VirtualList` | same |
| `VirtualGrid` | `VirtualGrid` | exactly one `.map` template | `VirtualGrid` | same |
| `ErrorBoundary` | `ErrorBoundary` | exactly one element | none (`ref` rejected) | same |
| `CustomElement` | `CustomElement` | none | `CustomElement` | same |

Library components (`Form`, `Field`, `Checkbox`, ...) and project components are not intrinsics
(subset spec section 11); they lower to `Component` nodes.

## 4. Lowering rules shared by all elements

1. **Node.** Each element becomes one IR node whose `kind` is the table value, `source` is the element
   span. For an alias the span is that of the lowercase element as written.
2. **Attributes in source order.** Spread attributes expand first (subset spec 10.2); duplicates after
   expansion: the later one wins, except that a duplicate written explicitly twice is
   `TESSERA_JSX_PROP_CONFLICT`.
3. **Plain props.** An attribute that is not reserved lowers to `props.<name>` with the name as
   written (camelCase, `aria-*` keeps its hyphens). A literal is written as the bare JSON value; an
   expression as its IR Expression object. Enum literals lower to the string.
4. **Booleans.** `disabled` (flag form) is `true`. `disabled={false}` still lowers to
   `props.disabled = false` (a literal `false` is kept, so that the designer can show the value).
5. **Text.** Children text runs follow the rule of [`elements-basic.md`](elements-basic.md) section 0.
6. **Events.** `onX={handler}` lowers to `events.onX` holding a `bind::` or `handler::` id
   (ir-v1 section 2.1). The handler is checked against the payload of the event row.
7. **`style`** keys become kebab-case CSS names and numbers become `"<n>px"` for length properties
   (jsx-ir-lowering decision 10). `className` lowers to `props.className`.
8. **`key`** lowers to the node `key`; `ref` to a `refs[]` entry whose `target` is the node id.
9. **Conditionals and loops** lower to `condition` and `loop` on the node, as in subset spec 10.4/10.5.
   They are not attributes and are not listed per element. They are not allowed on the single child of
   `ErrorBoundary` and on the template child of `VirtualList`/`VirtualGrid` beyond the loop itself.
10. **Determinism.** Output depends only on the source text and the table. Props are emitted in sorted
    key order by the IR writer; the table order never leaks into output.
