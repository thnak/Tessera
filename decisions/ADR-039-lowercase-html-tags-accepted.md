# ADR-039: Lowercase HTML tags are accepted as aliases of the intrinsic elements

- Status: Accepted
- Date: 2026-11-09
- Reverses: the rule "no lowercase HTML tag aliases" recorded in
  [`jsx-subset-v1.md`](../docs/specs/jsx-subset-v1.md) sections 10.1, 12 and 18 (a project-owner
  decision of the first version of that spec; it was not an ADR).
- Related: [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) (own JSX/TSX front end;
  React-like model), [`jsx-intrinsics-v1`](../docs/specs/jsx-intrinsics-v1/README.md) (the mapping
  table, normative).

## Context

The subset spec rejected lowercase tags (`<div>`, `<span>`) with `TESSERA_JSX_SUBSET_HTML_TAG` and
a suggestion of the closest intrinsic. The goal of ADR-036 is that React developers are productive
and existing React projects port easily. Every React project writes lowercase tags; rejecting them
made each ported file need a mechanical rewrite that a compiler can do without ambiguity.

## Decision

1. A lowercase tag that appears in the alias table of
   [`jsx-intrinsics-v1/html-aliases.md`](../docs/specs/jsx-intrinsics-v1/html-aliases.md) is accepted
   and resolves to an intrinsic (`div` to `View`, `span`, `p`, `label`, `h1` to `h6` to `Text`,
   `button` to `Button`, `input` to `TextInput`, `textarea` to `TextArea`, `img` to `Image`, and the
   other rows of the table).
2. The table is data. It also lists the HTML attributes that are rewritten for each alias
   (`input type`, `autoComplete`, `img width`, ...) and the HTML tags that have no sane mapping.
3. A lowercase tag that is not in the table, or is marked unsupported, is still
   `TESSERA_JSX_SUBSET_HTML_TAG`. The code is kept for exactly this case and its suggestion names
   the closest alternative from the table.
4. An alias is only spelling. It adds no default styles and does not promise the rendering of a
   browser ([ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) item 2: UI-like, not
   pixel-identical).
5. Resolution happens in analysis, not in the parser. The AST keeps the tag as written (spans and
   designer text patches stay exact); IR contains only the closed node kinds.
6. Capitalized names keep their meaning; a component name must still start with an uppercase letter
   (`TESSERA_JSX_SUBSET_COMPONENT_NAME`), so aliases never collide with components.

## Consequences

- Most React JSX ports without renaming tags. Attribute and event differences remain and are
  diagnosed with suggestions.
- The parser stops reporting `TESSERA_JSX_SUBSET_HTML_TAG`; the analysis pass reports it. A code
  slice is needed (listed in
  [`react-compat.md`](../docs/specs/jsx-intrinsics-v1/react-compat.md) section 5). Until it lands,
  the compiler still rejects lowercase tags.
- Editors need typings for the alias tags (`JSX.IntrinsicElements`), generated from the same table.
- The alias table is a maintained surface: adding a row changes the table, its tests and the
  typings together.

## Alternatives

- Keep rejecting lowercase tags: rejected by the owner (the cost to every port is high, the
  ambiguity is zero for the mapped tags).
- Map every HTML tag: rejected; tags with no equivalent (`table`, `select`, `video`, `form`) would
  compile into something that behaves differently from the browser. They stay errors with advice.
- Add UA default styles per alias (bold `strong`, heading sizes): rejected for V1; it would put
  inline-style precedence into aliases. See open question 12 of the intrinsics spec.

## Follow-up

- Code slice: analysis-side alias resolution, parser change, tests (see the list in
  `react-compat.md` section 5).
- Typings generator emits the alias tags.
