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
| Status: Diagnostics | a cell with "not run yet" or counts | **kept**, as the Problems item (counts, toggles the panel area) | VS Code's Problems item |
| Status: caret and selection | absent | **added**: `Ln, Col (n selected)`, runs Go to Line | the essential editor fact |
| Status: language | absent | **added** (TSX, CSS, JSON) | VS Code's language mode |
| Status: bell | in the toolbar | **moved** to the status bar's right end, history opens upward | VS Code puts notifications there |
| Toolbar row | its own row under the menu bar | **merged** into the menu bar row | one chrome row |
| Toolbar: Theme | icon button | **removed**; `View > Appearance`, `view.cycleTheme` and the palette keep it | rarely changed |
| Toolbar: Restart, Save, Share, device chip, palette | kept | **kept** | the quick actions that matter (Restart is the one primary action) |
| Panel headings ("Code", "Project", "Canvas") | uppercase labels over each panel | **hidden visually**, kept for assistive technology | the dock tab already names the panel |
| Editor hints ("Editing path", "Ctrl+Enter applies now") | two lines under the editor | **hidden visually**, kept as live text for assistive technology and the gates | the tab and the Run menu say it |
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

## What stays data-driven

The menu tree, toolbar and status bar items are `menus.json` (`menus`, `toolbar`, `statusBar`); command ids are
append-only (`command-ids.json`): none were added or renamed, `file.samples` and `help.about` changed behaviour only.
React draws what the registry says is live; the caret and the Problems count are written without a render.
