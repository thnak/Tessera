# Studio workbench core: registry, keymap, menus, palette

Status: W1 (issue [#555](https://github.com/thnak/Tessera/issues/555)), W2 (#557) and W3 (#558) implemented.
The normative rules (record fields, when-clause grammar, the menu tree, the slice plan)
are in [studio-workbench-v1.md](../specs/studio-workbench-v1.md); this note records how the code in
`designer/web/src/workbench/` realises them and where it deliberately differs.

## 1. The rule: data plus pure logic

`designer/web/src/workbench/` is plain ES modules and JSON. It imports no `react`, `react-dom`,
`@codemirror/*`, dock library or DOM global, so a Node gate runs every rule without a browser and the V1
rebuild of the studio with Tessera reuses it unchanged. `scripts/ci/preview_workbench.mjs` enforces the
import rule by scanning the module sources. The modules are `.js` with an `index.d.ts`, the same pattern as
`lib/share.js` and `lib/preview-state.js`.

| File | Owns |
|---|---|
| `when.js` | the when-clause grammar: parse, `evaluate`, specificity, referenced keys |
| `context.js` | the closed vocabulary of context keys and the change-notifying store |
| `keys.js` | chord parsing, the browser-reserved list, the resolver and the keymap |
| `registry.js` | record validation, `register`, `run`, `describe`, integrity checks |
| `commands.json` | one record per user-invokable action |
| `command-ids.json` | the committed, append-only id list |

## 2. Commands

`commands.json` holds the records of spec section 2.1. A command is `live` (a handler is registered),
`reserved` (the id and menu slot are claimed; hidden and not invokable) or `removed` (kept for its
`replacedBy`). `registry.run(id, args)` never throws and never rejects; it returns
`{ ok: true }` or `{ ok: false, code, message }` with the codes `unknown`, `removed`, `reserved`,
`no-handler`, `disabled`, `invalid-args` and `failed`. A throwing handler is logged and isolated.

Three checks keep the data honest, all in the Node gate: the records validate (ids well formed and unique,
every clause parses and names only published context keys, no chord is browser-reserved); live commands and
handlers are equal sets (a reserved command with a handler fails); and every id in `command-ids.json` still
exists, in the same order, with new ids appended there in the same change.

### 2.1 Where the code extends the spec

- `keybinding.also` lists extra default chords. The spec says one default per command but also binds both
  `Ctrl+Shift+P` and `F1` for the palette (and `Ctrl+Y` with `Ctrl+Shift+Z` for redo); `also` is how both
  are data.
- Radio and toggle items derive `checked` from the item's literal arguments: the clause may read
  `arg.<name>`, so `device.setViewport` is `device.viewport == arg.value` and one command renders a whole
  radio group.
- Added context keys: `tree.focus`, `project.multiComponent`, `device.profile|skin|viewport|dpr|fold|keepState`,
  `ui.theme`, `run.picker|inspector|enabled|component`. They exist so toggles and radios read page state.
- `args` properties may carry `x-provider`, naming a page-registered choice provider (`recent`, `skins`,
  `profiles`, `viewports`, `dprs`, `components`) so the palette can offer choices for an argument.
- Worker-service readiness disables a command only when the page publishes `service.<name>.ready` as
  `false`; an unpublished key does not (page-side services are always ready).

## 3. Keybindings

Chords are `Mod+Shift+P`; a sequence is space separated (`Mod+K Mod+O`). `resolve(event, pending, context)`
returns `match`, `pending` (waiting for the next step) or `none`. Ranking among bindings whose `when`
passes: a user override first, then the most specific `when` (the number of tests in the clause), then the
later declaration. An override whose command starts with `-` removes that command's default chord.

Registration rejects the chords Chromium reserves (`Mod+N`, `Mod+T`, `Mod+W`, `Mod+Shift+N`,
`Mod+Shift+T`, `Ctrl+Tab`, `Ctrl+PageUp/PageDown`, `Mod+1..9`, `Alt+Left/Right`) and names the fallback:
`Ctrl+Alt+N` for New File and `Alt+W` for Close Editor. A user override on a reserved chord is dropped and
listed in `keymap.rejected`.

## 4. Menus and toolbar (W2, issue #557)

`menus.json` is the menu tree of spec section 3.2 as data: items name a command and literal arguments, a
`submenu` nests, a `dynamic` entry names a provider the page registers (`recent`, `components`, `profiles`,
`skins`, `viewports`, `dprs`) and the command each entry runs. `menus.js` builds the visible tree from the
registry: a reserved command hides its item (so `Go` is absent until its first command is live), separators
fall between groups only, and an item with a `checked` clause is a `check` (no arguments) or a `radio` (with
arguments). A disabled item keeps its reason for the tooltip. A submenu with no entries is hidden unless it
sets `showEmpty` (Open Recent stays visible and disabled).

`menu-nav.js` is the whole keyboard model as a pure reducer (WAI-ARIA menu bar): Left/Right across the bar,
Down/Enter/Space open, Up/Down wrap and skip separators, Home/End, Right/Left into and out of submenus,
type-ahead on the first letter, Escape one level at a time (the root level closes onto the bar button, the
bar itself returns focus to where it was), Tab closes, and a disabled item is focusable but inert.
`Alt+letter` opens a menu by its mnemonic when no command owns the chord.

The React side is `regions/MenuBar.tsx` (`useMenuController` is shared by the bar, the device chip popover
and, in W3, context menus), `regions/WorkbenchChrome.tsx` (title bar, toolbar, device chip) and
`page-workbench.ts` (the one registry, the handlers, the context keys, chord routing). `App.tsx` has one mount
point: `useWorkbench({...})` and `<WorkbenchChrome />`. A phone (below 720 CSS px) shows one menu button whose
popup lists the menus as submenus, and the toolbar keeps Restart (and the palette button once W3 lands).

Where the spec's mapping (section 4.1) lands today: Open folder, Open zip, Reopen (Open Recent), Samples and
Load sample (one `file.samples`), New file, Rename, Delete, Download and Save moved out of `ProjectBar`;
Restart app moved out of the code column; Copy share link and the five device selectors moved out of the
header and the preview column. The Picker, Inspector and Run input buttons stay in the canvas and scene panes
(they act on those panes) and the Run menu items click them. `ProjectBar` keeps the project name, the status
note, the draft offer and the conflict choice. Developer-run browser scripts that selected the old controls
(`#control-viewport`, `#control-foldPosture`) drive the same commands through `window.__tesseraWorkbench.run`.

The gate for the model is `scripts/ci/preview_workbench_menus.mjs`; the browser check is
`scripts/ci/preview_browser_workbench.mjs` (developer-run, Playwright).

## 5. Command palette, Quick Open, Go to Line, context menus (W3, issue #558)

One overlay (`regions/CommandPalette.tsx`) serves all three searches; the input's prefix picks the mode, as in
VS Code. `Ctrl+Shift+P` and `F1` open it with `>` (commands), `Ctrl+P` with nothing (files), `Ctrl+G` with `:`
(a line). Typing `>` or `:` in any of them switches mode; `name:12` opens the file at line 12. The pure parts are
`workbench/palette.js`, gated by `scripts/ci/preview_workbench_palette.mjs`:

- **Ranking.** A fuzzy subsequence score with fzy's weights (consecutive runs, word starts, slashes, camel
  humps, small gap penalties). A file is also scored on its basename, which wins when it is the better match
  (plus a bonus), so `home` finds `ui/Home.tsx` before `styles/homepage-extra.css`. Ties break by recency, then
  shorter text, then alphabetically. An empty query lists recent items first (most recent first), then the rest
  alphabetically; an argument choice list keeps the schema's order until the person types.
- **Recent lists.** Commands (8) and files (12), most recent first, deduplicated, kept in `localStorage`
  (`tessera.studio.recentCommands`, `tessera.studio.recentFiles`); an unreadable value is an empty list.
- **What is offered.** Live, runnable commands on the `palette` surface, labelled `Category: Title` with their
  chord. A reserved command, or one whose `enabled` clause is false right now, is not listed.
- **Argument prompts.** A command with required `args` opens one step per missing property, from its JSON Schema:
  a property with an `enum` or an `x-provider` is a choice list (the provider is the same one the menus use), a
  number is typed and checked against `type`, `minimum` and `maximum`, a string against `pattern`. A refusal
  keeps the prompt open with the reason (`aria-invalid`, read from the polite status line); Backspace on an
  empty answer goes back a step. The finished arguments are handed to `registry.run`, which validates them
  again.
- **ARIA.** The input is a `combobox` controlling a `listbox` of `option`s; focus stays in the input and
  `aria-activedescendant` names the highlighted option. Escape closes and returns focus to where it was; a blur
  closes; `Ctrl+S` is ignored while the palette has focus (`focus.palette`).
- **Context menus.** `menus.json` `contexts` (`tree.item`, `editor`, `panel.tab`) open on a right click or on
  `Shift+F10` / the Menu key at the focused element (`regions/ContextMenuHost.tsx`), through the same menu
  model and keyboard handling as the menu bar. A right click on a file row selects it first. A context with no
  live item opens nothing, so the browser's own menu appears: `panel.tab` is that today, because its items
  (maximize, move, close) are reserved until the dock lands.

### 5.1 Keyboard

VS Code chords, with the browser fallbacks of the spec (section 2.5). `Mod` is Ctrl (Cmd on macOS).

| Chord | Command | Chord | Command |
|---|---|---|---|
| `Ctrl+Shift+P`, `F1` | Command Palette | `Ctrl+P` | Quick Open |
| `Ctrl+G` | Go to Line | `Ctrl+Shift+E` | Explorer (focus the tree) |
| `Ctrl+K Ctrl+O` | Open Folder | `Ctrl+Alt+N` | New File (`Ctrl+N` is the browser's) |
| `Ctrl+S` | Save | `Ctrl+Shift+S` | Download Project |
| `F2` (tree) | Rename File | `Delete` (tree) | Delete File |
| `Ctrl+Z`, `Ctrl+Y` / `Ctrl+Shift+Z` | Undo, Redo | `Ctrl+A` | Select All |
| `Ctrl+F`, `F3`, `Shift+F3`, `Ctrl+H` | Find, Next, Previous, Replace | `Ctrl+/` | Toggle Line Comment |
| `Ctrl+Shift+F5` | Restart App | `Ctrl+Enter` | Apply Now |
| `Alt+F E V R D H` | open a menu | `Shift+F10`, Menu key | context menu |

`Help > Keyboard Shortcuts` opens the palette in command mode, where every command shows its chord. The
reserved commands (`go.*`, `edit.format`, `edit.renameSymbol`, `view.problems`, `view.search`, the dock's
`view.*`) bind nothing until their owning issue flips them live. In the editor, CodeMirror handles
`Ctrl+Z`, `Ctrl+F`, `Ctrl+S` and the rest first and the workbench skips a key the editor already consumed; the
two CodeMirror search bindings that collided with VS Code (`Ctrl+G` and `Shift+Ctrl+G`) are removed so Go to Line
works with the editor focused.
