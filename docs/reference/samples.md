# Samples: what each one shows

ADR-063 decision 5: a sample exists to prove Tessera carries a complex UI stack at a cost close to React + Vite, so each one ships real author CSS and says what it proves. This page maps a React + Vite developer's checklist (issue #607) to the sample that carries each item. A gap is listed, not hidden.

| Item | Sample | State |
|---|---|---|
| Global sheet next to a CSS module, tokens as `:root` custom properties and `var()` | Showcase (`app.css`, `showcase.module.css`) | shown |
| `@media` (narrow window folds the stat grid and the actions row) | Showcase | shown |
| `:hover`, `:active`, `:focus-visible` | Showcase (buttons, tabs, link, input) | shown |
| Flexbox, grid, `gap`, `position` | Showcase, DashPage, FormPage | shown |
| Headings, paragraphs, lists, links as a browser draws them | Showcase | shown (global `h1`, `h3`, `a` selectors) |
| `useState`, conditional rendering, controlled input | Showcase | shown (typing in the Studio: #601) |
| Library components (`Button`, `Field`, `Form`) | FormPage, Gallery | shown |
| `@layer`, `@container`, theme dark/light | Gallery, SafeAreaPage | partly; Showcase to follow |
| `transition`, `@keyframes`, springs | none yet | needs #603 to play in the Studio |
| Lists with `.map`, custom components with props | none yet | the preview CLI cannot take a second component per file; to follow |

Browser twins: `tools/parity/showcase.html`, `form.html`, `dash.html`. Compare a page with its twin using `scripts/ua_audit/page.mjs` ([`ua-audit.md`](ua-audit.md), Page parity).
