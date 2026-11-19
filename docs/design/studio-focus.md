# Studio focus pass: code and preview first

Owner feedback on PR #586 asked the web studio (`designer/web`) to look like a code editor with a preview, not a
dashboard: less chrome, nothing decorative, one deliberate accent. This note records what was removed, moved or
kept and why. The normative parts live in [`studio-workbench-v1.md`](../specs/studio-workbench-v1.md) (menus,
toolbar, status bar); the core as built is [`studio-workbench-core.md`](studio-workbench-core.md).

## Before and after

| Element | Before | After | Why |
|---|---|---|---|
| Samples bar | a horizontal strip of buttons above the workspace, opened on first view | **removed**. Samples are in `File > Open Sample` (a quick pick, palette intent `samples`), in the palette, and in the welcome view the editor column shows while no project is open | a permanent bar for something used once per project |
| Title and logo | "T Tessera Studio" in the menu row | **moved** to `Help > About` (`help.about`, an accessible modal with the description, build and links). One visually hidden `h1` stays | the name is not a control |
| Footer description | a paragraph about the three Wasm modules under the status line | **moved** into About | prose in permanent chrome |
| Status: build id | "Build: ..." cell | **moved** to About | rarely needed |
| Status: compile target | "Compile target: path" cell | **moved** to the language cell's tooltip | the tooltip names the file and the entry Run compiles |
| Status: frame | "Frame: ..." cell | **removed** from the bar; the Canvas panel keeps its own frame stats | duplicated |
| Status: Diagnostics | a cell with "not run yet" or counts | **now the Problems item**: split error and warning counts with separate icons (`⊗ 1  ⚠ 2`, dashes before the first compile); a click opens and focuses the Problems panel | VS Code's Problems item |
| Status: caret and selection | absent | **added**: `Ln, Col (n selected)`, runs Go to Line | the essential editor fact |
| Status: language | absent | **added** (TSX, CSS, JSON) | VS Code's language mode |
| Status: bell | in the toolbar | **moved** to the status bar's right end, history opens upward | VS Code puts notifications there |
| Toolbar row | its own row under the menu bar | **merged** into the menu bar row | one chrome row |
| Toolbar: Theme | icon button | **removed**; `View > Appearance`, `view.cycleTheme` and the palette keep it | rarely changed |
| Toolbar: Restart, Save, device chip, palette | kept | **kept** | the quick actions that matter (Restart is the one primary action) |
| Toolbar: Share | icon button, copied a `#p5=` link | **removed entirely** (see below) | there is no cloud storage to share to |
| Panel headings ("Code", "Project", "Canvas") | uppercase labels over each panel | **hidden visually**, kept for assistive technology | the dock tab already names the panel |
| Editor hints ("Editing path", "Ctrl+Enter applies now") | two lines under the editor | **deleted** (`#editor-hint`, `#editor-file`); the active tab's `title` and `aria-selected` name the file, the Run menu names the chord | a hidden line is still a line to maintain |
| Empty editor tab strip | a grey bar with no tabs | **not drawn** while the welcome view shows | nothing to show |

## Accent

One hue, named by role (tokens in `designer/web/src/styles/base.css`, light and dark):

| Token | Used for |
|---|---|
| `--accent`, `--accent-text` | the primary Restart action (a filled button) |
| `--accent-fg` | accent as text on a pane (links, picked rows) |
| `--focus-ring` | every keyboard focus outline |
| `--tab-indicator` | the line over the active editor tab and dock tab |
| `--selection-bg`, `--selection-fg` | the highlighted menu item and palette option |
| `--status-bg`, `--status-fg` | the status bar, the page's one band of emphasis (VS Code's coloured status bar) |

`scripts/ci/studio_accent_contrast.mjs` computes the WCAG ratios of every pair (4.5:1 for text, 3:1 for
indicators) and fails when the OS-preference tokens and the explicit `data-theme` override drift apart.

## Share is gone

Owner decision: the studio has no cloud storage to share to, so the Share feature was removed, not hidden.

| Removed | Where it was |
|---|---|
| the toolbar button `#share` and its status line | `menus.json` `toolbar`, `WorkbenchChrome.tsx`, `page-state.ts` |
| the command `file.share` ("Copy Share Link") | `commands.json`, `page-commands.ts`, the File menu |
| the `#p5=` codec and its validator | `lib/share.js`, `share.d.ts`, `normalizeState`/`ShareError` in `preview-state.js` |
| the startup fragment decode and the broken-link banner (`#link-note`) | `lib/preferences.ts`, `page-startup.ts`, `App.tsx` |
| `useSampleAndShare`'s share half (the file is now `page-sample.ts`, `useSample`) | `page-sample-share.ts` |
| the codec's round-trip gate | `scripts/ci/preview_share_roundtrip.mjs` and its `pages.yml` steps |

Kept on purpose: `File > Open Sample` (and the welcome view), the theme command (`View > Appearance`, palette), and the
redirect stubs for the pre-merge `scene.html`/`canvas.html`, which still forward the query and a `#p5=` fragment
byte for byte; the page now ignores the fragment. Every gate that used `#share` as a focus anchor uses the Restart
button (`#restart-app`, always focusable: the toolbar uses `aria-disabled`, not `disabled`).

**Retired id.** `file.share` stays in `command-ids.json` (append-only) and in `commands.json` as
`status: "removed"`, `replacedBy: null`; `register` refuses a handler for it and `run` answers `removed`. The
convention already existed (`workbench/registry.js`); the workbench spec now says that `replacedBy` is an id or `null`.

## Problems panel

Diagnostics used to be a pane plus a status-bar text cell. It is now also a **real dock panel**, like Visual Studio's
Error List or VS Code's Problems (`panels.json` id `problems`, the id the layout spec had reserved):

- A bottom-dock tab, **first** in the group that holds `diagnostics`, active by default; the bottom dock stays
  collapsed by default. It moves, splits and closes like every other dock tab.
- Rows: severity icon, code, message, `file:line:col`; three severity chips (Errors, Warnings, Info) filter the list
  and carry the counts; arrow keys move between rows. Clicking a row opens its file and selects its span in the editor
  (a row without a byte offset jumps by line).
- `view.problems` (the reserved id, now live; `Ctrl+Shift+M`): shows and focuses the panel; pressed again from
  inside it, hides it. The status bar item runs it.
- The counts come from the **severity of each entry of the diagnostics array** the Diagnostics session already
  produces (`lib/problems.ts`). The session's output (its list, its status line, its JSON) is unchanged; it only
  hands the array over after each compile (`onDiagnostics`). With no project open the compiler's "no entry file"
  answer is not listed.
- **A hidden panel does no work.** The panel subscribes to the store only while it is the visible tab of an expanded
  group; a compile while the dock is collapsed assigns one pointer. Only the status item re-renders on a compile.
- **Layout version 2.** `migrations[1]` inserts `problems` first in the Diagnostics group of a stored version 1 layout
  (unless the viewer closed it), keeping the active tab and the collapsed state ([layout spec](../specs/studio-workbench-layout-v1.md) section 2.2).

Not done, deliberately: floating windows. The owner asked for "movable/floatable like other tabs"; Problems is
exactly as movable as every other panel, and the studio has no pop-out for any panel (a pop-out would re-parent
the canvas into another document; main spec decision 8).

## What stays data-driven

The menu tree, toolbar and status bar items are `menus.json` (`menus`, `toolbar`, `statusBar`); command ids are
append-only (`command-ids.json`): none were added or renamed. `file.share` was retired (above); `view.problems`
went from `reserved` to `live`; `file.samples` and `help.about` changed behaviour only. React draws what the
registry says is live; the caret is written without a render, and the Problems counts re-render only their own
status item.

**A stale menu bug this surfaced.** The menus are built during App's render from the registry's context, which
was published in an effect after that render, so a check mark showed the *previous* render's state until an
unrelated render came along (the Toggle Panel check mark only looked right because opening the panel area also
changed the active view). `describe(id, args, overrides)` now takes the page's fresh context for the build.
