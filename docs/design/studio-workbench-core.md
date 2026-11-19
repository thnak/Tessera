# Studio workbench core: registry, keymap, menus, palette

Status: W1 implemented (issue [#555](https://github.com/thnak/Tessera/issues/555)); W2 and W3 are added
below as they land. The normative rules (record fields, when-clause grammar, the menu tree, the slice plan)
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
