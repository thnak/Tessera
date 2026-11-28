## Studio: split editor groups and dragging a tab between groups (#1025)

### What changed
- `designer/web/editor-groups.js`: pure model. `side` is a subset of the workspace's `open` tabs, so close/delete/rename can't strand a tab; `active` stays the focused group's file (compile, status, history unchanged); the unfocused group's file is remembered by `settle`.
- `page-workspace.ts`: `splitEditor`, `moveTab`, `focusTab`, `joinEditors`; every update goes through `settle`; close/delete pick a neighbour in the same group. `acceptNewFile` moved to `lib/project.ts` to keep the file under 500 lines.
- `regions/EditorTabs.tsx` (new): tab strip with HTML5 drag (custom type) and Alt+ArrowLeft/Right; `regions/CodeRegion.tsx`: one `EditorGroup` (strip + own CodeMirror editor) per group; `sourceRef`/`__tesseraEditor` follow focus; edits are written to the group's own path. `createCodeEditor` takes an `id`.
- Docs: `docs/design/studio-page.md` section 3.2. CI: Node gate added to `pages.yml`.

### Verification
- `node scripts/ci/preview_editor_groups.mjs` (11 cases), `preview_studio_smoke.mjs`, `preview_page_checks.mjs`, `tsc --noEmit`: pass.
- `node scripts/ci/workbench_editor_groups.mjs --site designer/web/dist` (Playwright, developer-run): 23/23. Split shows two files, drag moves a dirty tab (dot and text kept), Alt+Arrow, close/merge end the split, Canvas node unchanged.
- `python -B scripts/gates.py --quiet`: 15/15. No C++ changed.

### What is left
Vertical/nested splits, persisting groups across reloads, a registered `view.splitEditor` command and menu entry (needs the commands/menus/MCP projection tables), same file in both groups.

Closes #1025
