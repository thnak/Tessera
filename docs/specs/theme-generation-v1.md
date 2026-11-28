# Theme generation from one seed color v1

Status: normative for the generator behind `tessera theme --seed` and
`tessera::ui::style::generate_theme_asset`. Decision:
[ADR-047](../../decisions/ADR-047-component-library-layers-and-design-language.md) item 5 (the
Instrument design language: neutral greys plus one saturated accent, light/dark/high-contrast, WCAG
2.2 AA for text, the accent never the only carrier of small text), issue
[#79](https://github.com/thnak/Tessera/issues/79). The token set this produces is an ordinary theme
asset: [`stylesheet-v1.md`](stylesheet-v1.md) section 9.1 for its shape,
[`theme-versions-v1.md`](theme-versions-v1.md) for `themeVersions` and pins. This document adds
only how the values are derived and what contrast each role must clear.

Implementation: `include/tessera/ui/style/{color_oklch,contrast,theme_generate}.hpp`,
`src/ui/style/{color_oklch,contrast,theme_generate,theme_generate_tokens,theme_generate_json}.cpp`,
CLI `tools/theme/src/main.cpp`.

## 1. Inputs and outputs

`generate_theme_asset(seed, options, specs)` takes:

| Input | Meaning |
|---|---|
| `seed` | Any color `parse_color` accepts: `#rgb`, `#rrggbb`, `rgb()`, `hsl()`, a named color |
| `options.id_prefix` | Theme ids become `<prefix>-light`, `<prefix>-dark`, `<prefix>-high-contrast`; must match `[a-z][a-z0-9-]*` |
| `options.version` | `MAJOR.MINOR.PATCH` written to `themeVersions`; empty writes an unversioned asset |
| `options.include_accent_scale` | Emit `--accent-50` … `--accent-950` (default true) |
| `specs` | The per-variant derivation table; `instrument_variant_specs()` is the Instrument one |

The output is a `ThemeAsset` with three themes sharing **one token name set in one order**, so a
component that reads a token in one variant finds it in the other two. `format_theme_asset` writes
the JSON (section 6). The same seed and options always produce the same bytes.

`generate_instrument_theme(seed, options)` is the same call with the Instrument table and the
default prefix and version.

## 2. Why OKLCH

sRGB interpolation between two brand colors passes through muddy greys and does not step evenly in
perceived lightness, so a generated ramp built in sRGB looks banded and a dark variant built by
"invert the light one" loses the accent's identity. OKLCH (Ottosson's OKLab in polar form) is
near-perceptually uniform in lightness and keeps hue stable while lightness moves, which is what a
tonal scale and a contrast solve both need.

The conversion lives in `color_oklch.hpp` and is written in the repo rather than pulled in: it is
two 3×3 matrix pairs and a cube root, and ADR-012 forbids a new dependency for that. It is defined
as:

- linear sRGB → OKLab → polar (chroma, hue in degrees), and the exact inverse;
- `max_chroma_in_gamut(lightness, hue)`, a bisection over the chroma axis, so a request that cannot
  be represented is **clamped in chroma** rather than having each channel clipped, which would
  shift the hue;
- 8-bit quantization, applied **before** every contrast measurement, so the checker measures the
  color the app will actually paint.

The unit test pins the conversion against published reference vectors for the sRGB primaries (red
`L 0.627955 C 0.257683 h 29.2339`, green `L 0.866440 C 0.294827 h 142.4953`, blue
`L 0.452014 C 0.313214 h 264.0520`), so a matrix typo cannot pass.

## 3. The derivation

Run in this order. Every color is quantized to 8-bit sRGB before it is measured or written.

1. **Read the seed.** Convert to OKLCH. A seed with chroma below **0.004** has no usable hue, so
   the accent hue falls back to the Instrument hue **254°**; the neutrality of the seed is
   preserved in the neutrals, not by emitting a grey accent.
2. **Accent chroma** is the seed's chroma clamped to **0.060 … 0.170**. The floor keeps a
   low-chroma seed from producing a grey "accent"; the ceiling keeps a very saturated seed from
   clipping against the sRGB boundary at the lighter and darker steps.
3. **Accent tonal scale.** Eleven fixed lightness steps:

   | step | 50 | 100 | 200 | 300 | 400 | 500 | 600 | 700 | 800 | 900 | 950 |
   |---|---|---|---|---|---|---|---|---|---|---|---|
   | L | 0.970 | 0.930 | 0.880 | 0.820 | 0.750 | 0.670 | 0.580 | 0.500 | 0.420 | 0.340 | 0.260 |

   Each step's chroma is the accent chroma scaled by `clamp(1 − 1.9·|L − 0.62|, 0.15, 1)`: full
   around L 0.62, tapering to 15 % at the ends where sRGB has no room, so the pale and dark steps
   do not flatten to grey.
4. **Surfaces** are neutrals at the seed's hue with a small chroma (`neutral_chroma` in the table):
   near-white for light, near-black for dark, white/white/near-white for high contrast.
5. **Solve each role** by walking OKLCH lightness outward from the role's target lightness in
   0.0025 steps, nearest first, and taking the first color that clears its floor (section 4).
   Walking outward rather than only darkening keeps the result as close to the intended look as the
   floor allows in either direction.
6. **The accent role** takes the scale step nearest its target that is a valid fill (its ink clears
   the text floor, and it clears the non-text floor on every surface). If no step qualifies — which
   the sweep never observed — it falls back to the same lightness solve as the other roles.
7. **Status colors** keep conventional hues regardless of the seed — danger 25°, success 148°,
   warning 85° — because a red that is not red is a worse failure than a theme that does not tint
   its errors. They solve to the same fill rule as the accent.
8. **Ink on a fill** is fixed per variant, not chosen by contrast: light and high contrast use
   white, dark uses black. Choosing it by contrast would let a mid-lightness accent flip polarity
   between seeds, so two seeds a few degrees apart could produce inverted buttons.
9. **Assemble and gate.** Quantize, write the tokens, then run `check_theme_contrast` over the
   finished asset. A finding is an error (`TESSERA_THEME_CONTRAST`), never a warning: the
   generator never returns a token set it has not checked.

Every solved pair clears its floor by 1.2 %, so an 8-bit rounding on either side of a comparison
cannot turn a pass into a fail.

## 4. Contrast: which role gets which threshold

WCAG 2.2 AA for normal text is **4.5:1** (SC 1.4.3) and for large text **3:1** (the same
criterion's large-text clause). Non-text components and indicators need **3:1** (SC 1.4.11). The
high-contrast variant is deliberately one step up the WCAG ladder, so it is **7:1 for normal text**
(AAA 1.4.6), **4.5:1 for large text** and **4.5:1 for non-text**. So yes: high contrast needs 7:1,
and this is where it applies.

The role table is `instrument_contrast_pairs()` in `contrast.hpp` — one table, used by the
generator to solve and by the checker to lint, so a role cannot be solved against one floor and
reported against another.

| Role | Pairs | Normal / high contrast |
|---|---|---|
| Normal text | `--on-surface`, `--on-surface-muted` on each of `--surface`, `--surface-raised`, `--surface-sunken`; `--on-accent` on `--accent`; `--on-danger`/`--on-success`/`--on-warning` on their fill; `--window-foreground` on `--window-background` | 4.5:1 / **7:1** |
| Large text | `--accent` on each surface | 3:1 / 4.5:1 |
| Non-text | `--focus-ring`, `--border-strong`, `--danger`, `--success`, `--warning` on each surface | 3:1 / 4.5:1 |

Three deliberate choices:

- **The accent is large text, never small text.** ADR-047 item 5 says the accent is used for fills
  and indicators and never as the only carrier of small text, so the table grants `--accent` the
  3:1 large-text floor and not the 4.5:1 normal-text one. A theme author who wants accent-colored
  body text must not rely on this table to bless it.
- **Text is checked on every surface, not just `--surface`**, because a control on a raised panel
  must be as legible as one on the page.
- **`--border` is intentionally absent.** It is a hairline divider, not a component boundary, and
  SC 1.4.11 does not require 3:1 for it; the shipped Instrument set sits at about 1.6:1 by design.
  `--border-strong` is the boundary role and is checked.

A theme that does not define `--surface` is checked only for the reserved chrome pair
(`stylesheet-v1.md` 9.1/9.2): the built-in `default` theme carries nothing else, and a project
theme may be chrome-only by design.

## 5. Determinism

The same seed and options produce byte-identical output. The only values written are strings (hex
colors, `px`/`ms` lengths, font stacks), so **no floating point is ever formatted** and there is no
rounding mode or locale to drift. The lightness search is a fixed 0.0025 grid with a fixed
tie-break (the darker step), not an open-ended solver; the JSON writer sorts nothing, because token
order is the documented order. Two runs are compared byte-for-byte by the CLI test and by the
golden test.

## 6. Output format

`format_theme_asset` writes `assets.theme` JSON with a two-space indent, one token per line, and a
trailing newline. `schemaVersion`, `start`, `themeVersions` and `themes` appear in that order;
themes follow the spec order; tokens follow the published Instrument order (surfaces and text,
then the accent family, then status, then the reserved chrome pair, then the static design tokens).
The implicit built-in `default` theme is never emitted — it exists only after parsing, and writing
it would replace the built-in chrome pair.

The token names the generator defines, beyond the static design tokens:

| Group | Tokens |
|---|---|
| Surfaces | `--surface`, `--surface-raised`, `--surface-sunken` |
| Text | `--on-surface`, `--on-surface-muted` |
| Lines | `--border` (hairline), `--border-strong` (boundary, 3:1) |
| Accent | `--accent`, `--on-accent`, `--accent-50` … `--accent-950` |
| Status | `--danger`, `--on-danger`, `--success`, `--on-success`, `--warning`, `--on-warning` |
| Focus | `--focus-ring` |
| Chrome | `--window-background`, `--window-foreground` (follow `--surface`/`--on-surface`) |

## 7. CLI

```
tessera theme --seed <color> [--out FILE] [--prefix <id>] [--version <semver>] [--no-accent-scale]
tessera theme --check FILE
```

`--seed` derives and writes (or prints, with no `--out`) a complete token set. `--check` runs the
same checker over an existing token set, generated or hand-edited, and exits 1 with one
`TESSERA_THEME_CONTRAST` line per failing pair; it is the lint the issue asks for, and the test
suite runs it over the shipped `instrument.theme.json`. Exit codes: 0 success, 1 contrast or
unsolvable seed, 2 usage or IO. The binary is `tessera_theme` so a future `tessera` driver can
forward arguments unchanged, the same shape as `tessera icons add`.

## 8. Errors

| Code | Cause |
|---|---|
| `TESSERA_THEME_SEED` | The seed is not a color `parse_color` accepts |
| `TESSERA_THEME_ID_PREFIX` | The id prefix, or a variant suffix, cannot form a valid theme id |
| `TESSERA_THEME_VERSION` | The version is not `MAJOR.MINOR.PATCH` |
| `TESSERA_THEME_UNSOLVABLE` | A role could not reach its floor anywhere in the lightness range |
| `TESSERA_THEME_CONTRAST` | The derived asset still misses a floor; the message names theme, pair, ratio and floor |

## 9. Evidence

- `tests/unit/color_oklch_tests.cpp`: the transfer functions, the published OKLCH reference
  vectors, the 8-bit round trip, gamut clamping through `max_chroma_in_gamut`, hex formatting and
  `parse_oklch`.
- `tests/unit/theme_contrast_tests.cpp`: relative luminance, the 21:1 and 4.5:1 reference ratios,
  the floors per role, the role table's completeness and its accent-as-large-text rule, a broken
  token set that is reported, a missing token that is reported, and the chrome-only exemption.
- `tests/unit/theme_generate_tests.cpp`: a hue-wheel sweep (72 seeds at three lightness levels plus
  the hard accents and the achromatic extremes), identical token sets across the three variants,
  every floor cleared, byte determinism, distinct documents per chromatic seed, the JSON round trip
  through the theme asset parser, and the option surface (`--prefix`, `--version`,
  `--no-accent-scale`) and error codes.
- `tests/unit/theme_golden_tests.cpp` with `tests/golden/theme/{orange,blue}.theme.json`: byte
  goldens for a saturated orange and pure blue.
- `theme_cli` (`tools/theme/run_cli_tests.cmake`): `--seed` writes and is byte-deterministic,
  `--check` accepts its own output and rejects a doctored copy with `TESSERA_THEME_CONTRAST`, an
  invalid seed and an invalid prefix exit non-zero, and the shipped `instrument.theme.json` passes
  the lint.
- `tests/unit/instrument_theme_tests.cpp` and `tests/unit/theme_version_tests.cpp` still cover the
  hand-maintained shipped asset; generated assets use the same checker.

### 9.1 The Studio token editor

The Studio Theme panel (#582, `view.theme`, `designer/web/{theme-tokens,theme-contrast}.js`) edits the
tokens of a `*.theme.json` asset or of `:root` custom properties in a stylesheet, one column per variant,
and shows the section 4 contrast table per variant (the high-contrast floors apply to ids containing
`high-contrast`). A write replaces exactly one value span, so comments, order and layout survive, and it is
dispatched as one editor transaction (Ctrl+Z, format and diff apply). A stylesheet edit repaints the Canvas
as a live apply; an asset edit is read at start, so it takes the restart path until the host reloads
assets live. Node gate: `scripts/ci/studio_theme.mjs`; browser gate: `scripts/ci/workbench_theme.mjs`.

## 10. Open questions

- The static design tokens (`--space-*`, `--radius-*`, typography, motion) are emitted from a table
  in `theme_generate_tokens.cpp` rather than derived from the seed. Density is the obvious next
  axis (ADR-032 stage 2), and it belongs to a redesign of that table, not to this generator.
- The high-contrast variant is light-on-white only. A dark high-contrast variant is a new
  `ThemeVariantSpec`, which the API already accepts; whether the design wants one is a design
  decision, not an implementation one.
