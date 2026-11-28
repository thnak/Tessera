# Icon

A component spec in the [template](_template.md) format (issue #530). It draws one icon of the emitted
set (`tessera icons add`, [component-library.md](../../design/component-library.md) section 3.1) and is
the first component of the library farm ([library-farm-plan.md](../../design/library-farm-plan.md)).

```text
component: Icon
family: Content
tier: 1
status: draft
implementation: partial (packages/tessera-ui/Icon.tsx, icon.css, icons/)
verified_by: [library_icon_lowering, library_icon_runtime, library_icon_style, codegen_vector_payload, icons_import, gallery_golden_icons_*]
```

## 1. Contract at a glance

| Aspect | Contract | Rules |
|---|---|---|
| Props | `name` (`<set>:<name>`), `label`, `size`, `mirror` | section 3 |
| Behavior | Draws the glyph in the inherited text color; draws nothing for an unknown name and keeps its box | FR1 to FR3 |
| Accessibility | Named: role `img` with that name. Unnamed: decorative, `aria-hidden`, absent from the tree | AR1, AR2 |
| Theming | One target part (`icon`); the color is the inherited `color`, never a prop | section 6 |
| Compatibility | A closed value is never removed in a minor release | AV1 |

## 2. Intent and ownership

An Icon is a small vector glyph. It is not a button (IconButton), an image (Image) or an animation
(AnimatedIcon).

- **Owns:** the glyph, its box (`sm` 16 px, `md` 20 px), the accessibility contract and the mirror hook.
- **Does not own:** the color (the parent's `color`), the layout around it, the icon data (the import
  tool and its license gate).

## 3. Public concepts

| Concept | Closed values or type | Default | Meaning | Invalid value |
|---|---|---|---|---|
| `name` | `<set>:<name>` of an imported icon | required | Which glyph | Draws nothing, keeps its box |
| `label` | text | `""` | The accessible name; empty makes the icon decorative | n/a |
| `size` | `sm`, `md` | `md` | 16 px or 20 px square | No size rule: the box has no width |
| `mirror` | boolean | false | The icon is directional (chevron, arrow) and flips in right-to-left layouts | n/a |

## 4. Behavior rules

| Id | Rule | Reason |
|---|---|---|
| FR1 | The glyph MUST paint with the node's computed `color` (a `currentColor` `Path`, tinted by the paint pass). There is no per-icon color prop. | An icon follows its parent's text color in every theme. |
| FR2 | `mirror` MUST flip the icon only where the computed `direction` is `rtl` (the engine's `--tessera-dir-sign` is `-1`); a non-directional icon MUST never flip. | The engine sets the signal itself, so no host sets a flip token. |
| FR3 | An icon the tool could not express as paths (shapes, groups, unknown attributes) MUST be absent from the glyph table, never redrawn differently. | Matches the importer's rule. |
| AV1 | A theme may not change which sizes exist. | Layout portability. |

## 5. Accessibility rules

| Id | Rule | Criterion |
|---|---|---|
| AR1 | A non-empty `label` makes the icon a `img` box named by it; the glyph under it is `aria-hidden`, so the tree holds one image per icon. | 1.1.1, 4.1.2 |
| AR2 | An empty `label` makes the icon decorative: a `presentation` box with `aria-hidden`, absent from the tree. | 1.1.1 |

## 6. Anatomy, parts and theming

| Part | Mapping | Notes |
|---|---|---|
| `icon` | target | The sized host box (`ticon`); carries size and mirror classes. |
| glyph | none (reason: intentional) | Drawing, not content; hidden from the tree. |

- **Variables:** `--tessera-dir-sign` (engine-provided: `-1` where `direction` is `rtl`, absent otherwise; read with fallback `1`).
- **Tokens read:** none; the color is inherited.

## 7. Relationships

- **Composed by:** IconButton, Link, Banner, EmptyState, Chip (planned), Button (`icon`).
- **Pipeline:** `tessera icons add` emits the SVG assets, `icons.g.hpp` and `IconGlyph.tsx` (one
  `currentColor` `Path` per path of each path-only icon). `Path` takes literal `d`, so the component
  selects among them by `name`. `currentColor` is a tint: codegen paints the document white and sets
  `PaintVectorStyle::tint_current_color`; mixing `currentColor` with a literal color on one `Path` is
  refused.

## 8. Verification map

| Rule | Test | States | Failure signal |
|---|---|---|---|
| FR1 | `codegen_vector_payload`, `library_icon_runtime` (tint flag on every glyph path), `gallery_golden_icons_*` | rest | Wrong color |
| FR2 | `library_icon_style` (flip token), `library_icon_runtime` (class on directional only), `gallery_golden_icons_rtl` | LTR, RTL | Flip on a plain icon |
| FR3 | `icons_import` (golden `IconGlyph.tsx` header lists `tabler:search`) | n/a | Icon redrawn |
| AR1, AR2 | `library_icon_runtime` (snapshot: one image named Home, no decorative nodes), `library_icon_lowering` | named, decorative | Unnamed image in the tree |
| Press in a Button | `library_icon_runtime` | rest | Click swallowed |
| Parity | Evidence gap: no Chrome twin yet | n/a | n/a |

## 9. Current state

Implemented: the component, the emitted set (`packages/tessera-ui/icons/`, from the license-gated
fixture subset in `tests/fixtures/icons` until a real Iconify pin is vendored), the gallery page
(`--page icons`) and goldens per theme plus RTL. Gaps:

- **Stroke caps and joins** are done: `Path` takes `strokeLinecap`, `strokeLinejoin` and `fillRule`, and
  the importer passes the SVG attributes (own or inherited from the root `<svg>`) through.
- **Chrome parity page** (`structure_diff_pct` under 0.5 %) is not done; the gate (#529) needs an HTML
  twin of the icon page.
- **Icons with shapes** are done for `circle`, `ellipse`, `rect` (with radii), `line`, `polyline` and
  `polygon`: the importer converts them to path data. Groups and transforms stay absent (FR3).
- **Direction** is done: the cascade sets `--tessera-dir-sign` from the computed `direction`, so the
  direction root sets nothing (`--icon-flip` is gone).

## 10. Decisions and open questions

- **DEC1:** `currentColor` is a tint, resolved in codegen and the paint pass, because ThorVG has no
  current color (ADR-057 keeps ThorVG behind the `PathRasterizer` seam, so this holds for an in-house
  rasterizer too).
- **DEC2:** the glyph table is generated TSX rather than a runtime asset lookup, because `Path` props
  are literals (jsx-intrinsics-v1 section 1).
