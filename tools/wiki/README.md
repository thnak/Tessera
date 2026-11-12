# Tessera wiki client

A static React client that renders the repository's own `docs/` and `decisions/` markdown, so the
documentation can be browsed early without a separate wiki repository. The content is never copied:
Vite bundles the markdown files straight from the repo, so the wiki cannot drift from the code.

Node.js is design-time tooling only ([dependencies](../../docs/specs/dependencies.md)); it is never a
build or runtime dependency of the native app.

## Run

```bash
cd tools/wiki
npm install
npm run dev        # regenerates the manifest, then serves with hot reload
npm run build      # manifest + theme check + typecheck + static build into dist/
npm run preview    # serve dist/
```

`dist/` is fully static with relative asset paths, so it can be hosted under any sub-path
(for example GitHub Pages).

## How it works

- `scripts/build-manifest.mjs` scans `docs/` and `decisions/` and writes
  `src/generated/manifest.json` (title, section, path). The file is git-ignored.
- `src/content.ts` lazy-loads each markdown file with `import.meta.glob`, one chunk per document.
- `src/Markdown.tsx` renders with `react-markdown` + GFM + heading slugs. Relative links to other
  documents become wiki routes, and links to source files or anything outside `docs/` and
  `decisions/` open on GitHub. Images under `docs/` are bundled.
- Routing is hash based (`#/doc/<repo path>?h=<heading>`), so no server rewrite rules are needed.

## Theme

`src/theme/tessera-theme.css` is written as a Tessera stylesheet: the V1 semantic priorities of
[ADR-016](../../decisions/ADR-016-stylesheet-engine.md) and
[stylesheet-v1](../../docs/specs/stylesheet-v1.md) only (custom properties, `@layer`, `@media`,
flexbox, box model, typography, colors). The goal is that the first-party UI demo can load the same
theme in the native engine. `npm run check:theme` fails the build if the file uses grid, `:has()`,
`@container`, `@scope`, `@property` or wide-gamut colors.

Design tokens live in the `tokens` layer as `--color-*`, `--space-*`, `--font-*` custom
properties; dark mode is a `@media (color-scheme: dark)` override of the same tokens.
