# Tessera wiki client

A static React client for the **Tessera GitHub Wiki** (<https://github.com/thnak/Tessera/wiki>), so the
guides can be browsed and themed early. The wiki holds **guides with examples**. Design docs, specs
and ADRs stay in the source repo under [`docs/`](../../docs/README.md) and
[`decisions/`](../../decisions/README.md); the client only links to them and does not render them.

Node.js is design-time tooling only ([dependencies](../../docs/specs/dependencies.md)); it is never a
build or runtime dependency of the native app.

## Run

```bash
cd tools/wiki
npm install
npm run dev        # fetch wiki, build manifest, serve with hot reload
npm run build      # fetch + manifest + theme check + typecheck + static build into dist/
npm run preview    # serve dist/
```

`dist/` is fully static with relative asset paths, so it can be hosted under any sub-path.

To preview unpushed pages, point at a local checkout of `Tessera.wiki.git`:

```bash
TESSERA_WIKI_DIR=../../../Tessera.wiki npm run dev      # PowerShell: $env:TESSERA_WIKI_DIR=...
```

## How it works

- `scripts/sync-wiki.mjs` shallow-clones (or fast-forwards) `Tessera.wiki.git` into `.wiki-cache/`
  and copies it to `wiki-content/` (both git-ignored). Offline, it falls back to the cache.
- `scripts/build-manifest.mjs` builds the navigation. `_Sidebar.md` sets order and sections
  (a `#` heading or `**bold**` line opens a section; `[[Page]]`, `[[Text|Page]]`, `[Text](Page)`
  add pages). Pages not in the sidebar go under "More". `Home` is the landing page.
- `src/Markdown.tsx` renders GitHub Wiki markdown: `[[Wiki Links]]`, relative page links with or
  without `.md`, heading anchors, GFM tables, bundled images. A link to a missing page opens the
  GitHub "create page" screen.
- Links starting with `docs/`, `decisions/`, `src/`, `include/`, `samples/`, `tools/`, `tests/`,
  `designer/` or `scripts/` open the file in the source repo, so a guide can point at the design
  rationale without copying it.
- Every page has an "Edit this page on GitHub" link. Routing is hash based
  (`#/page/<Name>?h=<heading>`), so no server rewrite rules are needed.

## Theme

`src/theme/tessera-theme.css` is written as a Tessera stylesheet: the V1 semantic priorities of
[ADR-016](../../decisions/ADR-016-stylesheet-engine.md) and
[stylesheet-v1](../../docs/specs/stylesheet-v1.md) only (custom properties, `@layer`, `@media`
with `min-`/`max-width` and `prefers-color-scheme`, flexbox, box model, typography, colors). The goal
is that the first-party UI demo can load the same theme in the native engine. `npm run check:theme`
fails the build on grid, `:has()`, `@container`, `@scope`, `@property`, wide-gamut colors,
`@media (color-scheme)` and media range syntax.

Design tokens live in the `tokens` layer as `--color-*`, `--space-*`, `--font-*` custom
properties; dark mode overrides the same tokens under `@media (prefers-color-scheme: dark)`.
