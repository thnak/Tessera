# Studio workbench v1: commands, menus, toolbar and slice plan

Status: library choice and decisions 1-10 and 13 Accepted by the owner 2026-11-19 (section 8);
decisions 11-12 remain open. Slice W0 is done; W1 (core) is implemented, see [studio-workbench-core.md](../design/studio-workbench-core.md); the studio today has loose buttons
(`designer/web/src/regions/`) and a fixed three-column layout.
Owner feedback 2026-11-19: (1) the page needs an IDE-style action bar with menus, (2) there is still
no real dock ([#516](https://github.com/thnak/Tessera/issues/516)).
Companion: [studio-workbench-layout-v1.md](studio-workbench-layout-v1.md) (dock model, hard
constraints, library choice). Parent: [designer-studio-v1.md](designer-studio-v1.md) section 3.
Decisions this builds on: [ADR-054](../../decisions/ADR-054-studio-front-end-react-vite.md) (React +
Vite is the stepping stone), [ADR-060](../../decisions/ADR-060-studio-services-in-web-workers.md)
(services are Web Workers behind one UI-agnostic protocol; Chromium only; the V1 studio is rebuilt
with Tessera).

## 1. Purpose and the one rule

The workbench is the studio's shell: menu bar, toolbar, dock, status line, command palette and
keyboard shortcuts. **The one rule: the workbench is DATA plus pure logic, and React is only a
renderer of it.** At V1 the studio UI is rebuilt with Tessera; if commands, menus and layout live in
React state, that rewrite throws them away. So three JSON documents and one pure module own the
shell, and a renderer (React today, a Tessera view later) reads them.

| Artifact | Format | Owns |
|---|---|---|
| `commands.json` | JSON, schema in section 2 | every user-invokable action |
| `menus.json` | JSON, section 3 | menu bar, submenus, context menus, toolbar quick actions |
| `panels.json` + layout document | JSON, [layout spec](studio-workbench-layout-v1.md) | what a panel is, where panels sit |
| `workbench-core` | plain TypeScript/JS, no DOM, no React | registry, when-clause evaluator, keybinding resolver, layout model operations, migrations |

Location (proposed): `designer/web/src/workbench/`. The pure module follows the precedent of
`lib/preview-state.js` and `lib/share.js`: Node gates import it without a browser, so every rule
below is testable headless. `workbench-core` may not import `react`, `react-dom`, `@codemirror/*` or a
dock library; a lint-style check in the preview gates enforces that.

Non-goals: a plugin or extension API; user-authored commands; a settings UI beyond keybindings and
layout (settings slice, section 7); theming the workbench beyond the existing light/dark.

## 2. Command registry

One registry feeds **five consumers**: the menu bar, the command palette, context menus, keyboard
shortcuts and MCP. A command that exists anywhere exists once, with one id. Menus, palette and
keybindings never carry behavior; they reference an id.

### 2.1 Command record

```json
{
  "id": "file.save",
  "title": "Save",
  "category": "File",
  "description": "Write the changed files back to the opened folder",
  "keybinding": { "default": "Mod+S", "when": "!focus.palette" },
  "enabled": "project.source == 'folder' && !project.busy",
  "visible": "project.open",
  "checked": null,
  "args": null,
  "services": ["project-io"],
  "surfaces": ["menu", "palette", "keybinding"],
  "mcp": null,
  "status": "live"
}
```

| Field | Meaning |
|---|---|
| `id` | `area.verb[Object]`, lower-camel segments, unique, never reused. Renaming an id is a breaking change for keybinding overrides and MCP, so ids are append-only; a retired id stays as `status: "removed"` with `replacedBy` |
| `title`, `category` | the label shown in the palette is `Category: Title`; menus show `title` only. `titleShort` is optional for the toolbar |
| `keybinding.default` | `Mod+S`, `Mod+Shift+P`, chords as `Mod+K Mod+O`. `Mod` is Ctrl on Windows/Linux and Cmd on macOS; keys are `KeyboardEvent.key` names normalized to upper case. At most one default per command; extras are user overrides |
| `keybinding.when` | a when-clause scoping the binding |
| `enabled`, `visible`, `checked` | when-clauses (section 2.2). `enabled` false greys the item; `visible` false removes it; `checked` makes it a toggle (`menuitemcheckbox`). Absent means true/false/not-a-toggle |
| `args` | `null`, or a JSON Schema 2020-12 object. A palette invocation with required args opens a quick-pick/input per argument; a menu item supplies literal args in `menus.json` (section 3.1); MCP uses the schema as the tool `inputSchema` |
| `services` | which [ADR-060](../../decisions/ADR-060-studio-services-in-web-workers.md) services the handler calls: `viewer`, `analysis`, `format`, `mock`, plus page-side `project-io`, `share`, `layout`, `prefs`. Used for readiness (a command whose service is not ready is disabled with a reason), for the failure-isolation rule and for the MCP rule below |
| `surfaces` | which consumers list it. Default is all but `mcp`. `palette` only keeps a command off menus |
| `mcp` | `null` (not exposed) or `{ "tool": "<snake_case name>" }` (section 2.4) |
| `status` | `live` (handler registered), `reserved` (id and slot claimed by a later slice; hidden and not invokable until a handler registers), `removed` |

### 2.2 When-clauses and context keys

Predicates are strings in a deliberately tiny grammar so a Tessera-built UI can evaluate the same
text: identifiers, string and number literals, `== != && || !` and parentheses; no calls, no
arithmetic. Evaluation is a pure function `evaluate(clause, context)` where `context` is a flat
record of **context keys** the page publishes. Unknown keys evaluate to `false` (never throw).

| Key | Type | Meaning |
|---|---|---|
| `project.open` | bool | a project is loaded (a sample counts) |
| `project.source` | `'sample' \| 'folder' \| 'zip' \| ''` | where it came from |
| `project.busy` | bool | an open/save is in flight |
| `project.dirtyCount` | number | files with unsaved edits |
| `project.hasRecent` | bool | a recent folder exists and is not the open one |
| `project.folderSupported` | bool | File System Access API is available |
| `editor.focus` | bool | the code editor has focus |
| `editor.hasSelection` | bool | selection non-empty |
| `canvas.available` | bool | WebGPU is available (false keeps Scene and Diagnostics alive, [#516](https://github.com/thnak/Tessera/issues/516)) |
| `canvas.running` | bool | the app is evaluated and painting |
| `panel.<id>.open` | bool | panel is in the layout (`panel.canvas.open`) |
| `panel.<id>.visible` | bool | panel is the active tab of a visible group |
| `layout.phone` | bool | below the phone breakpoint |
| `layout.isDefault` | bool | current layout equals the default |
| `device.foldable` | bool | the selected device or skin has a fold posture |
| `service.<name>.ready` | bool | the worker booted and answered its handshake |
| `focus.palette`, `focus.menu` | bool | transient modal focus owners |

### 2.3 Handlers, targets and execution

The data says what exists; a handler says what happens. `registry.register(id, handler)` is called
by the page (React today) at boot. Rules:

- **Id equality is checked**: a test fails when a `live` command has no handler, when a handler has no
  record, or when a menu item names an unknown id. A `reserved` command with a handler is an error
  (flip its status in the same change).
- **Handlers are async-safe and non-blocking.** `run(id, args?)` returns `Promise<Result>` where
  `Result` is `{ ok: true }` or `{ ok: false, code, message }`. A throwing handler becomes
  `{ ok: false }`, is logged to the diagnostics core, and never reaches another panel (failure
  isolation, layout spec section 4.3).
- **Focus-routed editor commands.** `edit.undo`, `edit.redo`, `edit.find`, `edit.selectAll` route to the
  focused editor through an `EditorTarget` interface; CodeMirror implements it today. The registry
  generates the CodeMirror keymap for editor-scoped commands from the same records, so `Mod+S` and
  `Mod+Enter` are defined once, not in `editor/code-editor.ts` and again here.
- **Keybinding resolution** is a pure function over (event, context): match chord state, then
  `when`, then most specific `when` wins, user override beats default. A reserved-key list
  (section 2.5) is enforced at registration.
- **Undo of layout/commands is not in scope**; `edit.undo` is the editor's history only.

### 2.4 MCP

`tessera-mcp` ([mcp-server-v1.md](mcp-server-v1.md)) is a native stdio process and cannot see
browser state. The registry therefore feeds MCP **by metadata, not by transport**: a command may set
`mcp.tool` only if (a) `services` is non-empty and contains no page-side-only service, (b) `args` is a
JSON Schema, and (c) its result is plain data. Such a command's `title`, `description`, `args` become
the tool's `description` and `inputSchema`; its handler is the same service call the studio makes
(ADR-060 protocol), so a UI action and an agent action cannot drift. A generator emits
`tools/mcp/schema/commands-tools.json`; a test requires it to match the registry byte for byte, the
same rule `tools.json` already has. Existing tool names are never renamed
([mcp-server-v1.md](mcp-server-v1.md) section 3); a registry command takes an existing name via
`mcp.tool`. UI-only commands (panels, layout, menus) are never exposed. This is a proposal because
it changes who owns tool schemas (open decision 5).

### 2.5 Keyboard reality in a browser

Owner direction 2026-11-19: **the reference is VS Code and Visual Studio; keep VS Code's default
keybindings wherever a command exists.** The tree in 3.2 therefore uses VS Code chords, and the
parity matrix (section 6) lists each. Where VS Code's chord is not capturable in a browser, the
fallback is named explicitly and flagged.

Chromium reserves some chords that `preventDefault` cannot capture. The registry rejects defaults on
`Mod+N`, `Mod+T`, `Mod+W`, `Mod+Shift+N`, `Mod+Shift+T`, `Ctrl+Tab`, `Ctrl+PageUp/PageDown`, `Mod+1..9`
and `Alt+Left/Right`. `Mod+S`, `Mod+O`, `Mod+P`, `Mod+F`, `F1`, `F2`, `F5`, `F6`, `F10`, `F12` and
`Mod+Shift+P` are capturable but collide with browser defaults, so the studio captures them only
while it owns focus and shows the chord in the menu so the owner can see what the page overrides. The
palette key `Mod+Shift+P` and `F1` are both bound (VS Code parity). VS Code's `Ctrl+N` (new file),
`Ctrl+W` (close editor), `Ctrl+T`, `Ctrl+Shift+N` and `Ctrl+Tab` are **not capturable** in Chromium
tabs; fallbacks are `Ctrl+Alt+N` (new file), `Alt+W` (close editor) and `Ctrl+PageDown/Up` is also
reserved, so there is no default for editor-tab cycling in a browser tab (open decision 11).

## 3. Menu bar model

### 3.1 Menu record

```json
{
  "menus": [
    { "id": "file", "title": "File", "mnemonic": "F", "items": [
      { "command": "file.openFolder", "group": "open@1" },
      { "submenu": "file.recent", "title": "Open Recent", "group": "open@3" },
      { "command": "file.save", "group": "save@1" },
      { "command": "device.setViewport", "args": { "value": "390x844" } },
      { "dynamic": "samples", "title": "Samples", "group": "open@4" }
    ] }
  ],
  "contexts": { "tree.item": [], "editor": [], "panel.tab": [], "canvas": [] },
  "toolbar": [ { "command": "run.restart", "emphasis": "primary" } ]
}
```

- Items are one of: `command` (optional literal `args`), `submenu`, `dynamic` (a provider id the page
  registers: `recent`, `samples`, `components`, `skins`, `devices`; the provider returns items at
  open time, so menus stay data while content is live), or a derived separator.
- `group` is `name@order`; a separator is drawn between groups, never authored. Within a group items
  sort by order then declaration.
- A radio set is a group of items whose command has `checked` clauses over the same key, rendered as
  `menuitemradio`.
- `contexts` are the context menus (right-click or `ContextMenu`/`Shift+F10`): `tree.item`, `editor`,
  `panel.tab`, `canvas`. Same item grammar.
- `toolbar` lists quick actions (section 4.2). The toolbar is a projection of commands, not a second
  place to define behavior.
- **Reserved items** point at `reserved` commands. They exist so the tree is stable and reviewable;
  the renderer hides them until the command is `live`.
- Titles ending in `...` open a dialog or picker. Mnemonics are Alt+letter, unique per level.

### 3.2 The menu tree

`*` = live today (a command wraps an existing control); `R:En` = reserved for an IDE slice;
`P:E3`..`P:E4` = reserved for the device-profile series ([studio-device-profiles-v1.md](studio-device-profiles-v1.md),
issues #446-#448). Chords are proposals (section 2.5 applies).

```text
File
  Open Folder...                *file.openFolder        Ctrl+K Ctrl+O (VS Code)
  Open Zip...                   *file.openZip           
  Open Recent >                 *file.recent            (one entry today: the last folder; list later)
  Open Sample >                 *file.samples           (replaces the Samples gallery toggle + Load sample)
  ------
  New File                      *file.newFile           Ctrl+Alt+N (VS Code Ctrl+N is reserved)
  Save                          *file.save              Ctrl+S
  Download Project (.zip)       *file.download          Ctrl+Shift+S (VS Code: Save As)
  ------
  Rename File...                *file.renameFile        F2 (tree focus)
  Delete File                   *file.deleteFile        Delete (tree focus)
  ------
  Copy Share Link               *file.share             (no VS Code analogue; unbound)
  Close Folder                  R:file.closeProject
Edit
  Undo / Redo                   *edit.undo/redo         Ctrl+Z / Ctrl+Y (editor focus; Ctrl+Shift+Z also)
  ------
  Cut / Copy / Paste            *edit.cut/copy/paste    native, shown for discoverability
  Select All                    *edit.selectAll         Ctrl+A (editor focus)
  ------
  Find...                       *edit.find              Ctrl+F
  Find Next / Previous          *edit.findNext/Previous F3 / Shift+F3
  Replace...                    *edit.replace           Ctrl+H
  ------
  Format Document               R:edit.format           Shift+Alt+F      (ADR-061, format service)
  Rename Symbol...              R:edit.renameSymbol     F2 (editor focus) (analysis service)
  Toggle Line Comment           *edit.toggleComment     Ctrl+/   (editor keymap)
View
  Command Palette...            *view.palette           Ctrl+Shift+P, F1
  Quick Open (go to file)...    *view.quickOpen         Ctrl+P
  Go to Line...                 *view.gotoLine          Ctrl+G  (also ':' in quick open)
  ------
  Explorer / Search / Source Control   *view.explorer Ctrl+Shift+E, R:view.search Ctrl+Shift+F,
                                R:view.sourceControl Ctrl+Shift+G   (activity bar views)
  Problems                      R:view.problems         Ctrl+Shift+M
  Toggle Side Bar               *view.toggleSideBar     Ctrl+B
  Toggle Panel                  *view.togglePanelArea   Ctrl+J
  Toggle Status Bar / Activity Bar  *view.toggleStatusBar / *view.toggleActivityBar
  Zen Mode                      *view.zenMode           Ctrl+K Z   (Esc Esc exits)
  Layout Preset >               *view.layoutPreset      Default, Editor Only, Preview Focus, Side by Side
  ------
  Appearance >                  Theme: System/Light/Dark  (*view.setTheme, radio)
  ------
  Panels >                      one checked item per panel (view.togglePanel):
                                  Project, Editor, Canvas, Scene, Diagnostics,
                                  Platform events*, Mock console*, Call trace*,
                                  R:Problems, R:Source Control
  Focus Next / Previous Region  *view.focusNextRegion   F6 / Shift+F6
  Move Panel >                  *view.movePanel         Left, Right, Bottom, Into Group >  (palette-first)
  Maximize / Restore Panel      *view.maximizePanel     (VS Code: no default; panel header button too)
  Reset Layout                  *view.resetLayout
Go
  Go to Definition              R:go.definition         F12
  Go to Symbol...               R:go.symbol             Ctrl+Shift+O
  Find All References           R:go.references         Shift+F12
Run
  Restart App                   *run.restart            Ctrl+Shift+F5 (VS Code: Restart; F5 is Run)
  Apply Now                     *run.applyNow           Ctrl+Enter (editor focus)
  Run Input Script              *run.inputScript        (Scene panel)
  ------
  Replay Recorded Session...    P:E4 run.replay
  ------
  Picker                        *run.togglePicker       (checked)
  Inspector                     *run.toggleInspector    (checked)
  Component >                   *run.component         (dynamic: only when the project has more than one)
Device
  Device Profile >              P:E3 device.profile     (dynamic: #447 profiles as data)
  Skin >                        *device.setSkin         None, Generic Desktop ..., Generic Phone ... (radio)
  Viewport >                    *device.setViewport     (radio over VIEWPORTS)
  Pixel Ratio >                 *device.setDpr          1x, 2x, 3x ... (radio)
  Fold Posture >                *device.setFold         None, Flat, Book, Tabletop (radio; enabled with device.foldable or always, see 4.3)
  ------
  Platform Events >             P:E4 device.platformEvent   (back, rotate, lock, ...)
Help
  Keyboard Shortcuts            *help.shortcuts
  Studio Documentation          *help.docs               (opens the docs site)
  Report an Issue               *help.reportIssue        (GitHub issue form, new tab)
  About Tessera Studio          *help.about              (build id, engine id, backend: the status line's data)
```

`Source Control` (R: `view.sourceControl`, `scm.*`) is reserved as a panel and a menu slot only.
`Terminal` and `Selection` menus are deliberately absent: there is no terminal in a browser studio, and
multi-cursor commands live in the editor keymap rather than a menu.

Six top-level menus are live (File, Edit, View, Run, Device, Help). `Go` is entirely reserved and
appears when its first `go.*` command lands.

## 4. From today's buttons to menus and toolbar

### 4.1 Mapping

Source column is the file that renders the control today.

| Today (control, id, file) | Becomes | Stays as toolbar quick action |
|---|---|---|
| Open folder `#open-folder`, `ProjectBar.tsx` | `file.openFolder` | no |
| Reopen `<name>` `#reopen-folder` | `File > Open Recent` | no |
| Open zip `#open-zip` | `file.openZip` | no |
| Save `#save-project` (shows `(n)` dirty count) | `file.save`; count moves to the tab/status line | yes, icon with dirty badge |
| Download `#download-project` | `file.download` | no |
| New file / Rename / Delete (`ProjectBar.tsx`) | `file.newFile`, `file.renameFile`, `file.deleteFile`; also the `tree.item` context menu | no |
| Samples `#toggle-gallery`, `PageHeader` | `File > Open Sample >`; the gallery region becomes the quick-pick behind it | no |
| Load sample `#load-sample`, `CodeRegion.tsx` | same command as above (duplicate removed) | no |
| Copy share link `#share` + `#share-status` | `file.share`; the status text moves to the status line | yes, icon |
| Restart app `#restart-app` | `run.restart` | yes, the single primary action |
| Picker / Inspector toggles `#pick-toggle`, `#inspect-toggle`, `CanvasPane.tsx` | `Run` menu checked items | yes, as Canvas panel header actions (they act on that panel) |
| Run input `#run-input`, `ViewPanes.tsx` | `run.inputScript` | stays as Scene panel header action |
| Component select `#component-select` | `Run > Component >` | no (shown only for multi-component projects) |
| Skin, Viewport, DPR, Theme, Fold `<select>`s, `EnvironmentToolbar` | `Device` and `View > Appearance` menus | yes, one compact **device chip** (section 4.2) |
| Preview tablist (Diagnostics/Scene/Canvas), `PreviewColumn.tsx` | panels in the dock; `View > Panels` | no (the dock's tab strips replace it) |
| Editor tab strip and its close buttons | stay: they are the editor panel's own content | n/a |
| Status line | stays; gains nothing from menus | n/a |

### 4.2 Toolbar quick actions

The toolbar is one row under the menu bar. It holds only what a person uses continuously:

1. **Restart** (primary, `run.restart`).
2. **Save** with dirty badge, **Share** (icon buttons).
3. **Device chip**: one button showing `<skin or profile> · <viewport> · <dpr>x`; click opens the Device
   menu as a popover. It replaces five `<select>`s. Its keyboard path is the Device menu.
4. **Theme toggle** (icon, cycles System/Light/Dark).
5. **Palette** button (a search-like field that opens the palette; VS Code's command center).

Everything else is menu-only. A quick action is a projection of a command: it shows the same
`enabled`/`checked` state and the same chord tooltip, and cannot exist without a command. At phone
width the toolbar keeps Restart and the palette button; the rest is reachable through the menu button
(section 5).

### 4.3 Fold posture

Fold posture stays always enabled, as the existing control comment explains (it is a layout input,
not tied to a foldable skin); `device.foldable` only affects ordering (the item moves to the top of the
Device menu for a foldable skin).

## 5. Wireframe

Desktop, default state (1440x900), VS Code shaped: activity bar, side bar, editor area, preview
group, status bar. The panel area starts **collapsed** (section 8, decision 10); the second drawing
shows it opened with `Ctrl+J`. The dock is specified in [the layout spec](studio-workbench-layout-v1.md).

```text
+----------------------------------------------------------------------------------------------+
| T  File  Edit  View  Go  Run  Device  Help               [ search / run a command (Ctrl+P) ] |  menu bar (role=menubar)
+----------------------------------------------------------------------------------------------+
| [> Restart] [Save 2] [Share] | [Pixel 8 . 390x844 . 3x v] [theme]                            |  toolbar (optional, view.toggleToolbar)
+--+-------------+-----------------------------------------+------------------------------------+
|[E]| EXPLORER    | App.tsx *  Home.tsx  theme.css   [x]    | Canvas | Scene              [pick]... |
|[S]|  ui/        | ui > App.tsx > App                      |------------------------------------|
|[G]|   App.tsx   |-----------------------------------------|     +--------------------+         |
|   |   Home.tsx  |  1  export function App() {             |     |  device skin       |         |
|   |  styles/    |  2    return <Column>...                |     |  engine canvas     |         |
|   |             |                                         |     +--------------------+         |
+--+-------------+-----------------------------------------+------------------------------------+
| WebGPU | main | diagnostics 2 (click opens the panel) | Ln 2, Col 5 | App.tsx | frame 2.1 ms     |  status bar (items run commands)
+----------------------------------------------------------------------------------------------+

 Ctrl+J opens the panel area (bottom, over the editor and preview columns):
+--+-------------+-----------------------------------------+------------------------------------+
|  | ...         |  editor                                  | preview                             |
|  |             +=========================================+====================================+  <- sash (snaps)
|  | PANEL: [Diagnostics 2] [Mock console] [Call trace] [Platform events]      [^ max][x]        |
|  |  App.tsx:12:5  error TSR1042 ...                                                             |
+--+----------------------------------------------------------------------------------------------+
```

Activity bar: Explorer (live), Search and Source Control (reserved placeholders, hidden until live).
Breadcrumbs line (`ui > App.tsx > App`) is the path today and gains symbols with the analysis
service. While dragging a panel, **drop-zone overlays** (centre = tab, four edges = split) show the
target region before release.

Phone width (390x844): the menu bar becomes one button; the dock shows exactly one group at a time
with a panel switcher; the persisted layout is untouched ([layout spec](studio-workbench-layout-v1.md) section 3.4).

```text
+--------------------------------+
| [=] Tessera Studio    [> ] [?] |  [=] opens the menu tree as a list
+--------------------------------+
| (Editor v)  Canvas  Diagnostics|  one visible group, tab-like switcher
|--------------------------------|
|                                |
|   active panel content         |
|                                |
+--------------------------------+
| WebGPU | diagnostics 2         |
+--------------------------------+
```

Region order for focus (F6 cycles, see layout spec section 4.4): menu bar, toolbar, dock groups in
reading order (left, centre, right, then bottom), status line.

## 6. Parity matrix vs VS Code / Visual Studio

Owner direction 2026-11-19: be as close to VS Code and Visual Studio as possible; their conventions
are the default reference. Status is **in the Studio today** (no workbench code has landed). Slice
names are section 7 (W*) and the IDE slices (E2-E8, numbering unconfirmed, open decision 6).
VS = Visual Studio. "Chord" = VS Code default unless noted.

| Feature | VS Code / VS behavior | Studio today | Slice |
|---|---|---|---|
| Title/menu bar | File Edit View Go Run ... Help, Alt mnemonics | none; loose buttons | W2 |
| Command palette | `Ctrl+Shift+P`, `F1`, `>` prefix | none | W3 |
| Quick open | `Ctrl+P`, fuzzy file search, `:line`, `@symbol` | none | W3 (files), IDE (symbols) |
| Go to line / symbol | `Ctrl+G`, `Ctrl+Shift+O` | CodeMirror has none bound | W3 / IDE |
| Activity bar | icon column switching side bar views | none | W5b |
| Side bar | Explorer, Search, Source Control, Run views | project tree only (fixed left column) | W5b |
| Explorer | tree, new/rename/delete, context menu | tree + buttons (#439) | W2 (menu), W3 (context menu) |
| Search (project-wide) | `Ctrl+Shift+F` | none | IDE (placeholder in W5b) |
| Source control | `Ctrl+Shift+G`, badges | none | IDE (placeholder in W5b) |
| Editor groups | splits, tabs, drag between groups | one editor, per-file tab strip (#431) | W5c (decision 12) |
| Preview (italic) tabs | single click opens a replaceable preview tab, edit or double click pins | none: click opens a permanent tab | W5c |
| Editor tab features | dirty dot, close, `Ctrl+W` | dirty dot, close | exists; chord W2 |
| Breadcrumbs | path > symbol above editor | none | W5c (path), IDE (symbols) |
| Panel area | bottom: Problems, Output, Terminal, Debug Console | none: Diagnostics is a tab in the preview column | W5 |
| Studio panel analogues | Problems = Diagnostics; Output = Mock console; Terminal = (none, no terminal in a browser); Debug Console / Call Stack = Call trace, Platform events | Diagnostics exists; others are #448 and mock-services | W5, #448 |
| Panel maximize/restore | header button, double-click tab strip | none | W5 |
| Drag-and-drop docking | drop-zone overlays (centre, edges), live preview of the target region | none | W5 (library) |
| Sash resize | drag, double-click resets, snap to default and to collapse | none (fixed CSS columns) | W5, W7 |
| Status bar | left: branch, problems; right: language, position; clickable items | exists (`StatusLine.tsx`), not clickable, no commands | W2 (items run commands) |
| Notifications / toasts | bell, toasts with actions, do-not-disturb | page `role="status"` lines scattered per region | W3b |
| Context menus | right-click, `Shift+F10` | none | W3 |
| Keybindings JSON | `keybindings.json`, conflict display, chords | hard-coded CodeMirror keymap | W8 |
| Settings UI | `Ctrl+,` search + JSON | `localStorage` preferences, no UI | W8 |
| Zen mode | `Ctrl+K Z`, hides chrome, `Esc Esc` | none | W7 |
| Layout presets | VS: Window > Apply Window Layout, Save; VS Code: layout picker, customize layout | none; reset only | W7 |
| Toggle side bar / panel | `Ctrl+B`, `Ctrl+J` | none | W5b |
| Focus cycling | `F6`, `Ctrl+1..3` groups | tab order only | W7 (`Ctrl+1` not capturable, use `F6`) |
| Panel keyboard move | VS Code: command "Move View/Panel To..." | none | W7 |
| Floating / pop-out windows | VS and VS Code: Move into new window | none | out of scope (decision 8) |
| Theme | color themes, light/dark/high contrast | light/dark/system | exists; picker W8 |
| Format / rename / go to definition / references / Problems | `Shift+Alt+F`, `F2`, `F12`, `Shift+F12`, `Ctrl+Shift+M` | none | IDE slices (reserved ids) |
| Terminal, debugger, extensions, remote | present | none; not applicable in a browser studio | not planned |

Reading the matrix: W2 gives the menu bar, toolbar, status-bar commands; W3 gives palette, quick
open, go to line, context menus, notifications; W5 the dock (panel area, maximize, drag-and-drop,
sashes); W5b the activity bar and side bar shell; W5c editor groups, preview tabs and breadcrumbs.

## 7. Slice plan

Each slice is behavior plus tests plus docs and lands as one reviewed unit (AGENTS.md "Landing
size"). Numbers are proposed issue titles; none is filed yet. "After" lists hard dependencies.

| # | Slice | After | Delivers |
|---|---|---|---|
| W0 | Library spike + ADR | this spec | decision recorded (layout spec section 5), `dependencies.md` and `THIRD_PARTY_NOTICES.md` entries, a throwaway Playwright proof on the real canvas |
| W1 | `workbench-core`: registry, when-clause evaluator, keybinding resolver, `commands.json` for today's actions, node tests | none | data model and tests with no UI change (existing buttons call `run(id)`) |
| W2 | Menu bar and toolbar renderers | W1 | `menus.json`, accessible menubar (APG pattern), device chip, buttons mapped per section 4.1, `StudioChrome`/`ProjectBar` loose buttons deleted; Playwright menu keyboard test; `preview_page_checks.mjs` ids updated |
| W3 | Command palette, quick open, go to line, context menus | W1, W2 | palette (quick-pick with args), `Ctrl+P`, `Ctrl+G`, `tree.item`/`editor`/`panel.tab` menus, `Ctrl+Shift+P`/`F1` |
| W3b | Notifications and toasts service | W1 | a `notify` command/service with actions and a bell history; replaces the per-region status lines |
| W4 | Layout model, panel registry, persistence, migrations (pure, no library) | W1 | JSON schema, `layout.default.json`, validator, migration table, node tests; adapter interface |
| W5 | Dock renderer (the library adapter), panel area (collapsed by default), maximize, drop-zone overlays, sash snapping, panel host contract | W4, W0, the CodeMirror editor panel ([#517](https://github.com/thnak/Tessera/issues/517)) | #516 acceptance: drag/split/tab/resize, reload restores, canvas never remounts, hidden never draws, phone collapse |
| W5b | Activity bar and side bar shell (Explorer live; Search and Source Control reserved), `Ctrl+B`/`Ctrl+J` | W5 | VS Code region model; side bar is a dock region holding views |
| W5c | Editor groups, preview tabs, breadcrumbs (path) | W5, editor panel | decision 12; may move after V1 |
| W6 | Rewrite `scripts/ci/preview_browser_layout.mjs` for the dock | W5 | layout spec section 4.5 |
| W7 | Keyboard model: F6 region cycle, move-panel commands, separator keyboard resize, zen mode, layout presets, a11y audit | W5, W3 | WAI-ARIA checks in the browser script |
| W8 | Keybindings JSON and settings UI | W3, W4 | `view.openKeybindings`, `view.openSettings` (`Ctrl+,`), JSON import/export of overrides and layout, conflict display |
| W9 | MCP projection | W1, ADR-060 services | `commands-tools.json` generator and equality test (open decision 5) |
| IDE slices (E2-E8) | Format, go to definition, rename, Problems panel, Source Control panel | W1/W2 for menu items; W5 for panels | each one flips `reserved` commands and panels to `live`; the menu tree does not change |

W1 and W4 are independent and can proceed in parallel. W2 delivers the owner's first complaint and
does not need the dock; W5 delivers the second. Neither blocks the engine-side B2 work.

## 8. Owner decisions

Owner review 2026-11-19: decisions 1-10 and 13 are **Accepted as recommended**; 11 and 12 stay open
with the working reading noted. Accepted decisions are the contract for the slices; changing one
needs a new owner decision, not a slice-level judgment.

1. **Dock dependency** (layout spec section 5): adopt flexlayout-react or dockview, or hand-build?
   Recommendation: flexlayout-react behind our own layout model, hand-built adapter retained as the
   exit. Needs a yes/no before W5.
    **Accepted**: flexlayout-react 0.11.2, exact-pinned, behind our layout JSON, operations and a thin adapter; dockview only if flexlayout fails a W0 check.
2. **Canonical layout format** is ours, not the library's. This costs an adapter in each direction.
   Recommendation: yes, because the V1 rebuild with Tessera must not inherit a library's JSON.
    **Accepted**: our JSON is the canonical layout format.
3. **Default chords** in the section 3.2 tree: VS Code's defaults are used wherever the browser allows (owner direction
   2026-11-19); the fallbacks are `Ctrl+Alt+N` for New File and `Alt+W` for Close Editor. Confirm.
    **Accepted**: VS Code default chords with the stated browser fallbacks `Ctrl+Alt+N` and `Alt+W`.
4. **Command id and keybinding stability promise**: ids are append-only from the first release.
   Is the studio's public surface stable enough to promise that before V1?
    **Accepted**: command ids are stable and append-only.
5. **MCP projection** (section 2.4): should the registry own the schema for studio-backed MCP tools, or
   stay separate as in `tools/mcp/schema/tools.json` today?
    **Accepted** (the registry owns the schema for studio-backed tools, as recommended; W9).
6. **Mapping of "E2-E8"**: the IDE roadmap numbering is not in this repository (the E2-E4 labels in
   the repo belong to the device-profile series, #446-#448). Reserved commands here use names, not
   numbers; please confirm or supply the roadmap so the slice table can cite the right issues.
    **Accepted** (reserved commands use names, not numbers, until the roadmap is supplied).
7. **Per-viewer only vs shareable layouts**: layout stays per viewer in `localStorage` and never in a
   share link or project file ([#516](https://github.com/thnak/Tessera/issues/516)). Should a project
   be able to suggest a default layout (e.g. a `tessera.json` `studio.layout` hint)? Recommendation:
   no for V1.
    **Accepted**: layout is per viewer only; no project layout hint in V1.
8. **Floating windows and pop-outs**: out of scope for V1 (Chromium supports them; a pop-out would
   re-parent the canvas into another document, which violates the no-remount rule). Confirm.
    **Accepted**: no floating windows or pop-outs in V1.
9. **Default layout wording**: #516's text says "Scene/Diagnostics as tabs"; the owner's later answer
   and the mock put Diagnostics in the bottom dock with Platform events, Mock console and Call trace,
   and Scene as a tab beside Canvas. The layout spec follows the later answer
   ([layout spec](studio-workbench-layout-v1.md) section 3.1). Confirm.
    **Accepted**: Diagnostics in the bottom dock, Scene beside Canvas.
10. **Default dock state COLLAPSED** (owner direction 2026-11-19): the panel area (Diagnostics, Mock
    console, Call trace, Platform events) starts hidden, opened by `Ctrl+J`, a status-bar item or any
    diagnostic arriving. This differs from the earlier #516 answer (bottom dock open); the layout spec
    follows the newest direction ([layout spec](studio-workbench-layout-v1.md) section 3.1). Does
    "collapsed" also mean the Scene tab and side bar start hidden? Assumed: only the panel area.
    **Accepted**: only the panel area is collapsed by default; the Diagnostics count sits in the status bar.
11. **Browser-reserved chords** (OPEN; working reading: accept the fallbacks, no PWA requirement): accept fallbacks, or require an installed-PWA window (where `Ctrl+N`,
    `Ctrl+W`, `Ctrl+Tab` are capturable) for full VS Code parity?
12. **Editor groups** (OPEN; working reading: W5c is after V1, W0-W7 do not depend on it) (splits of the editor area, section 6): W5c is a larger slice than #516 asks for;
    V1 or later? Preview tabs and breadcrumbs depend on it.
13. **Phone breakpoint**: 720 CSS px proposed; the existing layout check only pins 390 and 1440.
    **Accepted**: 720 CSS px.
