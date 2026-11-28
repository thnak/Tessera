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
| `@layer` | Demo (`demo.css`), Gallery | shown |
| Grid areas, `@container` (columns follow the container, not the window) | Grid (`samples/grid`) | shown |
| `transition`, `@keyframes`, `prefers-reduced-motion` | Motion (`samples/motion`), Counter, Demo | shown (the Studio plays them, #603) |
| Springs, `useAnimatedValue` | none | TODO(#603): do not play in the Studio yet; not faked |
| Keyed `.map` over a list state cell, conditional child (a loop item inside a handler is `TESSERA_IR_UNRESOLVED` today) | Lists (`samples/lists`) | shown |
| Custom components with props and children | none | the preview CLI cannot take a second component per file |
| Theme dark/light | Gallery, SafeAreaPage | partly |
| Own sheet on the two minimal samples (no fallback sheet) | Demo (`samples/compiler-demo`), Counter (`samples/parity-demo`) | shown; their gates stage the sheet (see below) |

The Demo and Counter TSX files are pinned by gates, so each imports a sheet that travels with it:
`codegen_cli_tests` copies `demo.css` beside every copy of `Demo.tsx`, `preview_parity_tests` lowers
`Counter.tsx` with `counter.css`, `preview_budget_probe.mjs` stages the sibling sheets next to the entry,
and `wasm_ir_parity.py` refuses a sample whose imported sheet is missing (both sides would otherwise
agree on the same missing-stylesheet diagnostic).

Browser twins: `tools/parity/showcase.html`, `form.html`, `dash.html`, `grid.html`, `motion.html`,
`lists.html`. Demo and Counter have no twin yet (their root is a `View`). Compare a page with its twin using `scripts/ua_audit/page.mjs` ([`ua-audit.md`](ua-audit.md), Page parity).
