# Studio keyboard guide

Everything the studio's dock does with a mouse has a keyboard path (WCAG 2.1.1). Each key below ends in the
same command or layout operation as the pointer gesture, so it works from the palette (`F1`) too. Specs:
[studio-workbench-v1.md](../specs/studio-workbench-v1.md) (commands), [studio-workbench-layout-v1.md](../specs/studio-workbench-layout-v1.md)
section 4.4 (semantics).

## Moving between regions

| Key | Command | What it does |
|---|---|---|
| `F6` / `Shift+F6` | `view.focusNextRegion` / `view.focusPreviousRegion` | Focus the next or previous region in reading order: menu bar, toolbar, each dock group (left to right, top to bottom), status bar. Hidden regions (zen mode, collapsed groups) are skipped; the ring wraps |
| `Ctrl+B` / `Ctrl+J` | `view.toggleSideBar` / `view.togglePanelArea` | Show or hide the side bar or the panel area; this is also how a collapsed group is restored without a mouse |
| Arrow keys, `Home`, `End` | (tab strip) | Move between the tabs of a group, one tab stop per strip |

## Searching the project

`Ctrl+Shift+F` (`view.search`) shows the Search panel, the side bar group's second tab, and focuses its query field. Results
refresh as you type.

| Key | Where | Result |
|---|---|---|
| `Enter` | query field | Search again now |
| `Alt+C` / `Alt+W` / `Alt+R` | anywhere in the panel | Match Case / Match Whole Word / Use Regular Expression |
| `Arrow` keys, `Home`, `End` | results | Move between file headings and results (one tab stop is enough; focus stays in the list) |
| `Enter` or `Space` | a result | Open the file with the match selected; on a file heading, fold or unfold it |
| `Ctrl+Alt+Enter` | anywhere in the panel | Replace All (when the Replace field is shown and the list is complete) |
| `Ctrl+Z` / `Ctrl+Shift+Z` | results (not a text field, which keeps the browser's undo) | Undo or redo the last replace, across all the files it touched; the Undo Replace and Redo buttons do the same |

The status line under the fields is a polite live region: it announces the result count, a pattern problem, and what a
replace or an undo did (including files left alone because they changed since). Locked files are not searched.

## Moving a panel

Focus a panel's tab, then:

| Key | Result |
|---|---|
| `Alt+Shift+Arrow` | Move the panel to the left, right, top or bottom of the dock (Down joins the panel area, opening it) |
| `Shift+F10` or the context-menu key | The tab menu: Maximize or Restore, Move Left / Right / Top / Bottom, Into Group (a list of groups named by their active panel), Close |
| `Delete` | Close the panel (when it is closable) |

After a move focus returns to the moved panel's tab and a polite live region says what happened ("Canvas moved
to the bottom"). Maximize and restore are `view.maximizePanel`; double-clicking a tab strip does the same.

## Resizing

Focus a separator (it is a tab stop with `role="separator"`, `aria-valuenow`, `aria-valuemin` and
`aria-valuemax` in percent):

| Key | Result |
|---|---|
| `Arrow` | Move the separator 16 px |
| `Shift+Arrow` | Move it 64 px |
| `Home` / `End` | Go to the minimum or the maximum the neighbours allow |

With a mouse a drag also snaps (to the default size, within 12 px of it, and to the minimum), double-click on a
separator resets that split to its default fractions, and dragging a group more than 24 px below its minimum
collapses it. A collapsed group keeps its size; drag its edge handle back, double-click the handle, or use the
toggle commands above to restore it. A collapse by key on a separator is not available.

## Zen mode and layouts

| Key | Command | What it does |
|---|---|---|
| `Ctrl+K Z` | `view.zenMode` | Hide the toolbar, status bar and menu bar. It does not touch `collapsed` or `maximized`, is never stored, and a reload never starts in it |
| `Esc Esc` | `view.exitZenMode` | Leave zen mode (also the notification's Exit Zen Mode button, or `F1` then Exit Zen Mode) |
| `F1`, then Layout Preset | `view.layoutPreset` | Apply `default`, `editor-only`, `preview-focus` or `side-by-side`; a notification offers one Undo layout change step |
| `F1`, then Reset Layout | `view.resetLayout` | Back to the default layout, also one undo step |

## Changing shortcuts

| Key | Command | What it does |
|---|---|---|
| `Ctrl+,` | `view.openSettings` | Open Settings: Keyboard shortcuts, Layout and Appearance tabs |
| `Ctrl+K Ctrl+S` | `view.openKeybindings` | Open Settings on the Keyboard shortcuts tab |

In the Keyboard shortcuts tab, **Change** on a row opens a field: press the new keys (or type `Mod+Shift+L`; a
sequence is `Mod+K Mod+O`), then **Set**. Enter sets, Escape cancels, **Remove** leaves the command with no
chord and **Reset** brings the default back. The old chord stops working when you set a new one. A chord that
another command in the same scope already uses is shown as a conflict on both rows. Browser-reserved chords
(`Ctrl+T`, `Ctrl+W`, `Ctrl+1..9`, ...) are refused with the reason. **Export JSON** downloads the overrides and
**Import** (a file or pasted text) replaces them; an invalid document is refused as a whole with the reasons and
nothing changes. The format is in [studio-workbench-v1.md, section 2.6](../specs/studio-workbench-v1.md#26-keybinding-overrides-and-the-settings-dialog-w8-issue-565).
The Layout tab exports and imports the layout and keeps named saved layouts. If the browser blocks storage, a
change lasts until the page is closed and the dialog says so.

## How it is tested

`scripts/ci/workbench_layout.mjs` and `workbench_dock_core.mjs` (Node, no browser) cover the operations and
the separator arithmetic. `scripts/ci/workbench_dock.mjs` and `workbench_commands_dock.mjs` (developer-run,
real Chromium) cover the ARIA semantics, the F6 ring over every region, moves with focus return and the live
message, zen not surviving a reload, presets with undo, and the mouse sash behaviors. `preview_workbench_keybindings.mjs` (Node) covers the override rules and `preview_browser_settings.mjs`
(developer-run) the dialog, a rebind surviving a reload, refused imports and blocked storage.
`workbench_search.mjs` (developer-run) drives
the Search panel above: the chord, the toggles, click to reveal, replace and its multi-file undo.
