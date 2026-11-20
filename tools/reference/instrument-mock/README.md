# Instrument reference mock

A small desktop-tool UI in **plain React + Vite** that shows the Instrument design language
([ADR-047](../../../decisions/ADR-047-component-library-layers-and-design-language.md)) on real screens,
pushed toward a **tactile hardware-instrument look** (raised keys, recessed wells, LCDs, knobs, a dark
band between light regions). It does **not** use Tessera (no runtime, no compiler); it runs in any
browser. It is the visual target for later Tessera ports, and its CSS is tagged so the engine features it
needs can be audited (see [tessera-gap tags](#tessera-gap-tags)).

Node.js is design-time tooling only ([dependencies](../../../docs/specs/dependencies.md)). The mock is
not part of the Pages build or CI and adds no package beyond the `tools/wiki` pins.

## Run

```bash
cd tools/reference/instrument-mock
npm install
npm run dev              # regenerate tokens, serve with hot reload
npm run build            # token check + typecheck + static build into dist/
npm run preview          # serve dist/
npm run check:tokens     # exit 1 if src/generated/tokens.css is stale
npm run check:contrast   # WCAG report per theme; exit 1 if a library or material pair fails
```

Deep links: `#/dashboard`, `#/settings`, `#/controls`, and `?theme=instrument-light|instrument-dark|
instrument-high-contrast` (for example `/?theme=instrument-dark#/settings`). The theme switch is the
LT/DK/HC segmented key in the top band.

## Tokens

`scripts/gen-tokens.mjs` writes `src/generated/tokens.css` from two files, one rule per theme on
`[data-theme]` (the start theme also on `:root`), values verbatim and in file order:

- [`instrument.theme.json`](../../../packages/tessera-ui/themes/instrument.theme.json), the versioned
  theme, **unchanged**;
- [`tokens/material.tokens.json`](tokens/material.tokens.json), this mock's **proposed material layer**
  (`--mat-*`): five surface levels, gradient pairs, shadow recipes as whole `box-shadow` lists, the lip,
  engrave and inverse-band colours, the signal colour and its glow, LED colours, extra radii (36 device,
  26 band, 14 plate, 9 key), the chamfer size, a focus ring that is not the accent, and a decoration
  switch. It has the same three theme ids and the generator rejects a theme with a different token set.

Component CSS reads `var(--token)` for every colour, with one exception: a few fixed rgba shadows and
highlights inside decoration (grille, screw, dark knob, inverse keys, status-band seams, the band's
top sheen), which are visible in the CSS and would become tokens in a real theme.

## Material recipes

All in [`src/styles/material.css`](src/styles/material.css), reused by every component:

| Recipe | Built from | Used for |
|---|---|---|
| RAISED `.m-raised` | `--mat-raised-a/b` 180deg gradient + `--mat-raised-shadow` (0 3px 6px drop, inset 0 1px 0 white highlight, inset 0 -1px 0 dark hairline); hover `--mat-raised-shadow-hover`, press `--mat-pressed-shadow` + 1px travel | keys, chosen tab/segment, slider and switch thumbs, current nav item |
| PLATE `.m-plate` | `--mat-level-0 -> --mat-level-1` + `--mat-plate-shadow`, 14px radius | panels |
| RECESS `.m-recess` | `--mat-recess-a/b` + `--mat-recess-shadow` (inset 0 1px 3px, 1px white lip below) | segmented trays, table header, badges, checkbox sockets |
| WELL `.m-well` | `--mat-well-a/b` near-black + `--mat-well-shadow` + scanline layer; readouts use `.m-glow` (signal colour + text glow) | LCDs, stat tiles, chart screen, meter, switch track, slider groove |
| ENGRAVE `.m-engrave`, `.m-seam` | 1px `--mat-engrave` line + 1px `--mat-lip` under it | section rules, row rules, seams between units, dividers |
| INVERSE `.m-inverse` (+ `.m-hatch`) | `--mat-inverse-a/b` 90deg gradient + inset top shadow; hatch adds a 115deg line pattern | top band, status band, macro band, save bar |

Decoration: corner screws (`.m-screw`), perforated grille (`.m-grille`), LEDs (`.m-led`), chamfered
corners (`.m-chamfer`), numbered index badges (`.m-index`), micro labels (`.m-micro`: 11px mono
uppercase, never below the label size, never a sub-AA colour). High contrast turns the recipes off
through the tokens: shadows `none`, equal gradient stops, 2px black edges (`--mat-edge*`),
`--mat-decor-opacity: 0` (screws, grilles, ticks, traces), no glow; the over-limit chart bars keep a
diagonal stripe so they do not rely on colour.

**Focus.** One rule in `base.css`: a 2px ring offset 2px, coloured `--mat-focus-ring` (near-black on
light, near-white on dark, `#0000D6` in high contrast) and `--mat-focus-ring-inverse` inside dark
regions. It is not the accent, so a focused primary key shows a ring (the theme's `--focus-ring` equals
`--accent`, 1.00:1). Checked by `check:contrast`.

## Screens

- **Shell:** the device chassis (36px radius, screws), a hatched dark band hanging from the top edge
  (chamfered mark, title, micro subtitle, a skewed glowing trace, LINK/REC/CAL LEDs, theme switch), a
  key column with index badges and LEDs plus a grille tab, a dark status band rising from the bottom.
  Below 768px the keys become a horizontal strip; below 480px the subtitle, mark and some status facts go.
- **Dashboard:** the hero console (three numbered units with LCD preset displays, step keys, power keys,
  knurled knobs with tick rings and glowing arcs, a DC/AC segmented switch, an L/R LED meter) joined by
  scalloped notches to a dark macro band of small knobs on connector trees; LCD stat tiles; tabs as a
  segmented well; a chart on an LCD screen; a table with engraved rules and LED badges.
- **Settings:** recessed fields with micro labels, fader slider with an LCD value chip, socket
  checkboxes and radios, slide switches with LED state, a hazard-striped danger zone, and a dark save bar.
- **Controls:** every key variant (now with `dark`) in every state, fields, selection, the instrument
  parts (knob, segmented, LCD, meter, inverse band), type scale, and the theme + material colour tokens.
- **Dialog:** a small device face with screws, an engraved head, and actions in a recessed tray.

[`screenshots/`](screenshots/) holds three JPEG captures (0.75 device scale). The layout was checked
in all three themes at 1280 and 420 CSS px with no horizontal overflow; the hero uses a container
query so it degrades by its own width (units stack, seams turn horizontal, one notch remains, grilles go).

## tessera-gap tags

Each CSS technique the Tessera engine may lack carries a `/* tessera-gap: <name> */` comment, so
`grep -rn "tessera-gap" src` lists every site. Tagged sites per technique (a comment may name two):

| Technique | Sites |
|---|---|
| `inset-shadow` (inset entries in a box-shadow list) | 43 |
| `radial-gradient` | 9 |
| `repeating-linear-gradient` (scanlines, hatch, LED segments, hazard, stripes) | 6 |
| `multiple-backgrounds` | 6 |
| `mask` (meter/LED segments, knob ring) | 5 |
| `repeating-conic-gradient` (tick ring, knurl, segmented arc) | 3 |
| `conic-gradient` (value arcs) | 2 |
| `mask-composite` (intersecting two masks) | 2 |
| `text-shadow` (LCD glow) | 2 |
| `repeating-radial-gradient` (machined cap) | 1 |
| `filter` (drop-shadow glow on the arc) | 1 |
| `clip-path` (chamfer polygon) | 1 |
| `svg-path` (scalloped notch, inline SVG) | 1 |
| `container-query` (hero layout) | 1 |

Untagged because Tessera already has them (or the equivalent): one `linear-gradient`, per-corner
radius, outer `box-shadow` lists (glows), `transform` rotate/skew/translate, `backdrop-filter`, dashed
and dotted borders, `opacity`.

## Porting rules the code follows

Function components, `useState`, a few `useRef`/`useEffect` (dialog, tab and segment focus, slider and
knob drag), plain elements, one CSS file per component, flexbox only. Knob, slider and meter pass
geometry to CSS only as custom properties or width/left. **Parts are class names**: root `t<name>`,
parts `t<name>-<part>`; each component's header comment lists them.

## Decisions taken while reworking

- **One accent, not the reference's amber.** The single saturated accent stays the theme's blue
  (`--accent`) for actions and selection; `--mat-signal` is its light tint for glowing readouts on
  dark wells. Amber appears only as `--mat-signal-warn`, the warning signal (over limit, REC, unsaved).
  Changing the hue is a token change.
- **Fonts:** nothing added. Display text is IBM Plex Sans with a synthesized 600 weight (only Regular
  ships); micro labels and values use the system monospace (no Plex Mono ships).
- **Contrast:** the reference's 9-10px `#8D93A3`/`#A9AEBB` labels would fail; micro labels here are
  11px in `--on-surface-muted` / `--mat-on-inverse-muted`, all at least 4.5:1 (7:1 in high contrast).

## Findings: Instrument on real screens

Status after the material layer (`resolved by material` means a `--mat-*` token covers it in this mock;
it is a proposal, not a theme change).

1. **No hairline that meets 3:1** (`--border` 1.33 to 1.58:1). *Partly resolved:* the material look
   separates regions by tone, so boxes need no outline; control edges use `--border-strong` on the field's
   bottom edge only. The theme still has no mid-strength control border.
2. **Focus ring equals the accent** (1.00 to 1.26:1 on a primary key). *Resolved by material:*
   `--mat-focus-ring` with an offset gap. The theme's `--focus-ring` is unchanged.
3. **Hover/pressed were opacity tricks.** *Resolved by material:* hover lifts (`--mat-raised-shadow-hover`),
   press sinks (`--mat-pressed-shadow` + travel). Remaining: dark-theme hover is subtle, since a bigger
   drop shadow on a near-black chassis barely shows; a hover tone step is still missing.
4. **Disabled was `opacity: 0.4`.** *Resolved by material:* a flat `--mat-level-3` key, muted text and a
   dashed edge. A named disabled token pair would still be better.
5. **No subtle status surfaces.** *Remains:* badges became recessed chips with LEDs, which sidesteps it.
6. **No data palette.** *Remains:* the chart is signal plus warning-signal; still no categorical set.
7. **Type scale stops at three sizes; no Plex Mono or other weights.** *Remains:* the mock uses 26px and
   24px sizes of its own and a synthesized 600.
8. **Spacing and component sizes.** *Partly resolved:* radii (36/26/14/9) and the chamfer are tokens now;
   knob (124/62), switch (44x24), socket (18), meter and band heights are still mock numbers.
9. **Elevation was one shadow, no scrim.** *Resolved by material:* raised, plate, recess, well, band
   and device recipes. Remaining: no scrim token (the dialog backdrop reuses `--shadow-floating-color`).
10. **Part naming has no convention.** *Remains.* `tbtn-primary`, `tbtn-pressed`, `tbtn-label` still look alike.
11. **Breakpoints cannot be used in `@media`.** *Remains:* the hero switched to a container query, which
    hits the same limit.
12. **No easing tokens.** *Remains.*
13. **High contrast is light-only, `--surface` = `--surface-raised`.** *Remains;* the material layer
    switches decoration off in high contrast and relies on 2px edges.
14. **No tokens for shell parts.** *Partly resolved:* bands, LEDs and index badges are recipes now;
    nav item height and status-band density are still literals.
15. **New with the material look:** shadow recipes live in tokens as whole `box-shadow` lists, which
    Tessera's token engine must accept as opaque values; several recipes depend on inset shadows and
    multi-layer backgrounds, the two most-used gaps above.

ADR-063 (browser-matching user-agent defaults) does not bite here: every element in the mock is
styled, and in a browser the defaults are the browser's own.
