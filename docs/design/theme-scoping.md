# Nested theme regions: `:has()` and `@scope` (issue #622)

Status: decided and implemented. Normative rules: [`stylesheet-v1.md`](../specs/stylesheet-v1.md)
sections 6 and 10. Motivation: [ADR-064](../../decisions/ADR-064-astryx-style-component-system.md)
item 6.

## 1. The decision

**Both are supported.** `:has()` is a selector the engine matches, and `@scope` is an at-rule the
engine evaluates. Neither is a named exception.

ADR-064 item 6 requires a theme to build to plain CSS custom properties inside `@layer`s, with no
theme runtime, and it names the one case that model cannot express on its own: **a nested theme
region**, a dark sidebar inside a light page. A token set is inherited, so a nested region is a
subtree that redefines a subset of the tokens and lets everything below it inherit the new values.
The two engine features that express that in standard CSS are:

- `@scope (<start>) to (<end>) { … }` — apply a rule only inside a region, which is how a theme
  styles "everything under this component" without a class on every descendant; and
- `:has(…)` — style a part by what it contains, which is how a component's own rule reaches a
  nested region it hosts (`Card:has(.theme-dark)`) without the theme knowing the component's
  internals.

Supporting both keeps the theme model plain CSS, which is the property ADR-064 item 6 is protecting:
a theme stays inspectable, ships as custom properties, and needs no runtime. The alternative — a
named exception with a different nested-theme mechanism — would have to invent a Tessera-only
construct (a `theme` attribute the cascade reads, or a scope token in the selector grammar). That
construct would be a second styling language beside CSS, would not be inspectable as CSS, and would
have to be taught to the compiler, the blob format and the designer. It buys nothing that `@scope`
does not already do, so it is rejected.

The cost is bounded and recorded below: `:has()` matching is the one selector form whose cost is not
a function of the node alone, and `@scope` adds a field to the compiled rule and to the blob.

## 2. `:has()`

- **Grammar.** `:has(<relative selector list>)`, parsed by the existing functional pseudo-class path
  (the argument is a selector list, so `:has(.a, .b)` is a disjunction). It is a pseudo-class, not a
  pseudo-element, and it may not be the subject of a `::part()` compound.
- **Matching.** A relative selector is matched against the node's **descendants** and its
  **following siblings** (and their descendants), which is the CSS Scoping 1 relative-selector
  anchor set for a non-`:scope` context. The argument's own combinators are honoured, so
  `:has(> .icon)` is a child and `:has(.icon)` is any descendant.
- **Specificity.** The most specific argument, as CSS says: `:has()` contributes the specificity of
  its most specific complex selector, not the sum. `:has(.a, #b)` is `(1,0,0)`.
- **`@supports selector(:has(a))`** answers true, because the engine matches it.
- **Cost, and the bound.** Every other selector form is decided by the node and its ancestors, so the
  index narrows the candidate rules and matching is O(depth). `:has()` looks *down*, so a rule
  carrying it cannot be narrowed by the node's own id/class/tag alone: the index still buckets it by
  its subject compound, and the descendant walk runs only for the candidates that already matched
  everything else. The walk is bounded by the subtree size, so the worst case for one node is
  `O(rules_with_has × subtree)`. That is the documented bound; a stylesheet that puts `:has()` on
  many rules over a deep tree is the case to measure before it grows. The index does **not** degrade
  to a full scan per node: a rule without `:has()` never walks a subtree, and a rule with it is
  still filtered by its subject bucket first.

## 3. `@scope`

- **Grammar.** `@scope (<start>) [to (<end>)] { … }`, where `<start>` and `<end>` are selector
  lists. A missing `to` means the scope runs to the end of the tree.
- **Application.** A rule inside the block applies to a node only when the node matches the block's
  own selector **and** the node is inside the scope: some ancestor (or the node itself) matches
  `<start>`, and no ancestor between that one and the node matches `<end>`. The scope is a filter on
  top of the selector, not a replacement for it, so `@scope (.card) { .title { … } }` styles a
  `.title` only under a `.card`.
- **`@layer`.** The two are orthogonal and compose in the order the cascade already defines: `@scope`
  decides *whether* a rule applies, `@layer` decides *where it sorts* among the rules that do. A
  `@scope` block inside `@layer base` keeps `base`'s slot, and a `@scope` block may contain an
  `@layer` block (and vice versa) because both are ordinary at-rule nesting.
- **Source order.** Unchanged: a scoped rule keeps the source order of its position in the sheet, so
  two scoped rules that both apply resolve by specificity and then by source order, exactly like
  unscoped ones. `@scope` adds no cascade key component.
- **Blob.** A scoped rule carries its start and end selector lists, so the compiled blob format
  gained a field and `format_version` moved to 2. There is no committed blob golden to regenerate;
  the round-trip tests cover it.

## 4. Tests

| Test | What it proves |
|---|---|
| `selector` | `:has()` parses, its specificity is the most specific argument, and a malformed argument fails the compound |
| `selector_match` | `:has()` matches a descendant, a child (`>`), a following sibling, and a disjunction; it does not match an ancestor or a preceding sibling |
| `supports` | `@supports selector(:has(a))` is true |
| `scope` | `@scope` parses with and without `to`, applies only inside the region, honours the end selector, composes with `@layer`, and keeps source order |
| `stylesheet_blob` | a scoped rule round-trips through the blob |
