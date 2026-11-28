## Studio: preview tabs and breadcrumbs (#562), dock verification (#516)

### What changed
- `designer/web/src/lib/editor-tabs.ts`: pure rules for the replaceable preview tab (open, pin, close) and the path breadcrumbs.
- Workspace state gains `preview`: a tree single click opens/replaces the italic preview tab; an edit, a tab double click or a tree row double click pins it; closing, deleting and renaming keep it consistent.
- `CodeRegion`: italic preview tab styling, double click to pin, a `#editor-breadcrumbs` path line above the editor.
- Gates: `scripts/ci/preview_editor_tabs.mjs` (Node, model via dist-ssr, wired in pages.yml) and `scripts/ci/preview_browser_editor_tabs.mjs` (Playwright: preview replace, pin by dblclick/edit, dirty, canvas element unchanged).
- Docs: workbench spec parity matrix and slice table, studio-page.md gate list.
- #516: already delivered by the W5 dock (`workbench_dock.mjs` covers open/close independently, drags, reload, no GPU on close, no re-init); nothing further needed.

### Verification
- `python -B scripts/gates.py --quiet`: 15/15.
- `tsc --noEmit`, `npm run build:chrome`, `preview_editor_tabs.mjs`, `preview_page_checks.mjs`, `preview_studio_smoke.mjs`, `workbench_dock.mjs` pass; `preview_browser_editor_tabs.mjs` 13/13 on Windows Chromium.
- Other browser gates (editor, workbench, problems, search, layout) show the same failures on unmodified main in this environment (no Wasm modules); not caused by this change.

### What is left
Split editor groups and dragging a tab between groups: #1025 (needs owner decision 12). #562 narrowed to that.

Closes #516
Refs #562
