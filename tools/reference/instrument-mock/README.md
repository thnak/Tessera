# Instrument reference mock

A small desktop-tool UI in **plain React + Vite** that uses the Instrument design language
([ADR-047](../../../decisions/ADR-047-component-library-layers-and-design-language.md)) on real screens.
It does **not** use Tessera (no runtime, no compiler); it runs in any browser. It exists so the owner
can judge the language before more library components are built, and so later Tessera ports of these
components have a browser twin to compare against (as `tools/parity/*.html` are twins of
`samples/gallery`).

Node.js is design-time tooling only ([dependencies](../../../docs/specs/dependencies.md)). The mock is
not part of the Pages build or CI.

## Run

```bash
cd tools/reference/instrument-mock
npm install
npm run dev              # regenerate tokens, serve with hot reload
npm run build            # token check + typecheck + static build into dist/
npm run preview          # serve dist/
npm run check:tokens     # exit 1 if src/generated/tokens.css is stale
npm run check:contrast   # WCAG report per theme; exit 1 if any pair fails (a report, not a gate)
```

Deep links: `#/dashboard`, `#/settings`, `#/controls`, and `?theme=instrument-light|instrument-dark|
instrument-high-contrast` (for example `/?theme=instrument-dark#/settings`).

## What is here

- **Tokens.** `scripts/gen-tokens.mjs` turns
  [`instrument.theme.json`](../../../packages/tessera-ui/themes/instrument.theme.json) into
  `src/generated/tokens.css`: one rule per theme on `[data-theme="<id>"]`, the start theme also on
  `:root`, values verbatim and in file order (deterministic). No token value is written by hand
  anywhere else; components read `var(--token)` only.
- **Button** imports the shipped [`button.css`](../../../packages/tessera-ui/button.css) unchanged, with
  the Tessera Button's class names. `button-extra.css` adds what a `<button>` element needs and the
  proposed focus ring (finding 6).
- **Font.** The repo's IBM Plex Sans Regular Latin subset (`designer/web/public/fonts`, OFL-1.1) is
  referenced in place, not copied. Nothing new is added.
- **Screens.** App shell (app bar with theme switch, sidebar navigation that becomes a strip below
  768px, status bar). *Dashboard*: four stat tiles, tabs for the time range, a column chart built from
  plain boxes with a limit line, a sortable table with status badges. *Settings*: text inputs with
  validation, select, slider, inline radio group, checkboxes, switches, a dirty-aware save bar and a
  danger zone that confirms in a dialog. *Controls*: every control in every state (hover, active and
  focus forced), the type scale and the live colour tokens of the active theme.
- **Overlay.** `Dialog` on the native `<dialog>` (focus trap, Escape, focus return); used for "New
  capture" and "Reset bench".
- [`screenshots/`](screenshots/) holds four reference captures (taken at 0.75 device scale).

## Porting rules the code follows

Function components, `useState`, a few `useRef`/`useEffect` (dialog open, tab focus, slider drag),
plain elements, one CSS file per component, flexbox only (no grid), no libraries. **Parts are class
names**: a root class `t<name>` and parts `t<name>-<part>` (for example `tslider-track`,
`tslider-thumb`, `tdialog-actions`); each component's header comment lists them. This follows
`button.css` (`tbtn`, `tbtn-primary`), which uses the same `-` for variants and states, so parts and
modifiers are not distinguishable by name (finding 10).

## Findings: Instrument on real screens

What looked weak, inconsistent or missing while building these screens. Contrast numbers are from
`npm run check:contrast` against theme version 1.0.0; the theme file was not changed.

1. **No hairline that meets 3:1.** `--border` is 1.58:1 on `--surface` (light), 1.45:1 (dark), so it can
   only be decorative. A control boundary must use `--border-strong` (4.09 to 4.88:1), which is heavy for
   inputs, table rules and panels. The gap: a "control border" token between the two.
2. **Focus ring equals the accent.** `--focus-ring` on `--accent` is 1.00:1 (light), 1.26:1 (dark),
   1.07:1 (high contrast). `button.css` draws a 2px ring that touches the control, so a focused
   primary button looks unfocused. Needs either a ring with a surface gap (what the mock does) or a
   distinct ring colour.
3. **Hover/pressed are opacity tricks.** `.tbtn:hover { opacity: 0.92 }` is barely visible on
   primary and danger (invisible in high contrast); in dark, the ghost/secondary hover moves to
   `--surface-sunken`, which is *darker* than the surface and reads as pressed. Missing:
   `--accent-hover`, `--accent-pressed`, `--danger-hover`, a neutral hover/pressed overlay.
4. **Disabled is `opacity: 0.4`.** No disabled foreground/background tokens; disabled primary text in
   dark is muddy, and opacity also fades the label and border of fields unevenly.
5. **No subtle status surfaces.** Only solid `--success/--warning/--danger` plus `on-*`. Badges,
   inline alerts, invalid-field backgrounds and selected rows want `*-subtle` backgrounds and an
   `--accent-subtle` / selected-row token. Badges here are outline-only for that reason.
6. **No data palette.** One accent and three status colours; a second series or a categorical chart
   has nothing to use. The chart uses accent plus warning for "over limit". Needs a 6-8 colour
   categorical set and a sequential ramp, checked for 3:1 against the surfaces.
7. **Type scale stops at three sizes** (11/14/18). No display or numeric size for KPI figures (the
   tiles use the title size), no heading levels, no weight tokens, and only Plex Sans Regular ships:
   no Medium/SemiBold and **no IBM Plex Mono**, although `--font-mono` names it. Monospace falls back
   to the system face (Consolas here), which changes the numerals' look by platform.
8. **Spacing scale has holes** (1,2,3,4,6,8 = 4..32px): no 2px step for hairline-adjacent nudges, no
   5 (20px) or 10/12 (40/48px) for page gutters and the app bar. Component sizes are missing too:
   checkbox/radio box, switch track/thumb, slider track/thumb, icon size, app bar, sidebar and status
   bar heights/widths are all mock-invented numbers.
9. **Elevation is one shadow.** `--shadow-floating-*` only; no scrim/backdrop token (the dialog reuses
   the shadow colour, which is a weak 0.45 black on the dark surface), no levels for menu vs dialog vs
   toast. `--z-*` exists but nothing names a sticky header/save bar.
10. **Part naming has no convention.** `tbtn-primary` (variant), `tbtn-pressed` (state) and a future
    `tbtn-label` (part) are indistinguishable. ADR-047 makes part names SemVer API, so a rule
    (for example `t<comp>__<part>`, `is-<state>`, `t<comp>--<variant>`) should be fixed before more
    components land.
11. **Breakpoints cannot be used.** `--breakpoint-*` are custom properties, which CSS does not allow in
    `@media`; every sheet must repeat 480/768/1200 as literals. Either generate media aliases or drop
    them from the token set.
12. **Motion has durations but no easing.** `--motion-*` gives 120/200/320ms; no `--ease-standard` /
    `--ease-exit`, so "short mechanical motion" has no shared curve.
13. **High contrast is light-only.** There is no dark high-contrast theme (Windows "High Contrast
    Black"); `--surface` and `--surface-raised` are identical in high contrast, so raised headers and
    tiles rely on the 2px borders alone.
14. **Missing shell parts.** Stage 2 lists AppBar/Sidebar/StatusBar/Tabs, but no tokens exist for the
    current-item indicator, nav item height, status-bar density, or the selected tab rule; this mock
    uses `--accent` with a 2px rule everywhere and that choice should be confirmed.

ADR-063 (browser-matching user-agent defaults) does not bite here: every element in the mock is
styled, and in a browser the defaults are the browser's own.
