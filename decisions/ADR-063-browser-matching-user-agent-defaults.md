# ADR-063: Default styles reproduce the browser's look as far as possible; exceptions are named

- Status: Accepted (2026-11-20, owner): "default style have to match browser behavior to avoid confuse
  and comparison", and "recreate the browser look as much as possible except when impossible". Supersedes
  [`react-compat.md`](../docs/specs/jsx-intrinsics-v1/react-compat.md) decision 12 (alias defaults as an
  opt-in `tessera-html-defaults.css`) and the "UA stylesheet parity" non-goal of `stylesheet-v1.md`.
- Date: 2026-11-20
- Related: [`stylesheet-v1.md`](../docs/specs/stylesheet-v1.md) sections 3 and 7.1,
  [ADR-047](ADR-047-component-library-layers-and-design-language.md) (library CSS in `@layer tessera-ui`),
  [`studio-page.md`](../docs/design/studio-page.md), issue #602

## Context

`stylesheet-v1.md` listed "UA stylesheet parity" as a non-goal and said the user-agent origin holds
"Tessera's small built-in defaults", but no user-agent sheet existed: `Origin::UserAgent` was ranked by the
cascade and nothing supplied it. A bare `<Button>Add</Button>` therefore drew its label at the top-left of
the box, where a browser's `<button>` centres it (#602). The library `Button` (`button.css`) centres its
label itself, so the same screen looked different depending on which `Button` an app used, and the
Studio's first sample showed the worse one. The people comparing a Tessera screen with a browser read
that difference as a bug in one of them.

## Decision

1. **There is one user-agent stylesheet**, `ui::style::ua_stylesheet_text()`
   (`include/tessera/ui/style/ua_stylesheet.hpp`), prepared once and appended to every cascade with
   `Origin::UserAgent`: the app host's (`AppHost::Impl::rebuild_cascade`) and the design-time preview's
   (`PreparedPreviewSheets::resolve`). The two share the same object, so they cannot disagree about a
   default.
2. **The principle is browser parity by default.** An element looks and lays out the way it does in a
   browser unless the author says otherwise. A difference is a bug in the engine, not a choice, unless
   it is **impossible** (no DOM, no browser quirks mode, a property the engine cannot implement, a
   platform control the OS owns). Each impossible case is listed as an exception with its reason, in
   the table that grows with the sheet (see Follow-up); an unexplained difference is not an exception.
3. **The sheet grows one verified rule at a time.** A rule is added when a difference is found, with a
   unit test that pins it (`tests/unit/ua_stylesheet_tests.cpp`). Quirks mode and vendor-prefix
   semantics stay non-goals. The first rule is `Button`: a flex row with both axes centred,
   `text-align: center`, `padding: 1px 6px` and `box-sizing: border-box` (a browser button is border-box, so an authored width is not grown by the padding). The alias defaults of decision 12 (heading sizes,
   bold, margins, list indentation) are rules of this same sheet, not a separate opt-in file, and
   nothing is injected inline.
4. **Authors always win.** The user-agent origin ranks below user, theme, author and inline for a normal
   declaration, so any author declaration overrides a default without a specificity contest. Important
   declarations keep CSS's reversed order.
5. **Sample projects show the full stack.** A sample exists to prove Tessera carries a complex UI at a
   cost close to a React + Vite project, so each ships real author CSS (a plain sheet and a CSS module,
   tokens, layers, media queries, animation) and exercises the features it names, instead of leaning on
   the built-in fallback sheet. The fallback stays only as the bare-bones look for a project with no CSS.
6. **The built-in preview fallback sheet is not a user-agent sheet.** `preview_stylesheet()` (sizes and
   colours used when a project has no CSS) stays an author-origin fallback; it gets no layout rules a
   user-agent default already provides. It also drops the fixed `height: 24px` it gave `Text`: a block with a fixed height puts its
   glyph at the top of an oversize box, so the label sat above the centre of any centred parent; the
   engine measures the line box instead, as a browser does (#602).

## Consequences

- A bare `Button` renders centred in the Studio and in a native build, and the Studio and native agree.
- Every golden and snapshot that renders a bare `Button` moves (its label's position). They are
  regenerated in the same change as the rule and reviewed as a diff of that one effect.
- The library `Button`'s own CSS is unchanged; it already sets what the user-agent rule sets.
- A future default is a deliberate edit of one file with a test, not an engine special case.

## Alternatives

- **Centre in the preview fallback sheet only.** Smallest, but a native app and an author sheet that
  styles `Button` without `display` would still differ from a browser. Rejected.
- **Move every sample to the library `Button`.** Hides the difference in the samples and leaves it in
  every other project. Rejected.
- **Import a browser's user-agent stylesheet wholesale.** Brings quirks and properties the engine does
  not implement. The rule-by-rule list reaches the same look without them, and keeps each exception
  explicit. Rejected.
- **Keep decision 12 (an opt-in `tessera-html-defaults.css`).** A default a developer must remember to
  import is the "differs from the browser" surprise this ADR removes. Rejected.

## Follow-up

- An audit of every element and control against a browser (headings, paragraphs, lists, links, inputs,
  `TextInput`/`TextArea`, form controls), producing the rules and the exceptions table in
  `stylesheet-v1.md` section 7.1.
- Rewrite the sample projects to carry real author CSS and the features they claim (decision 5), and
  check each in the Studio against the same markup in a browser.
