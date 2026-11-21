# Theme source v1: role tokens that build to plain CSS

Status: spike, off by default (issue #662, ADR-064 item 6). Nothing here runs in a shipped app: a
theme is built at authoring time and the app loads the resulting CSS or theme asset blob exactly as
it does today. The point of the spike is to measure the model, not to add a runtime.

Implementation: `include/tessera/ui/style/theme_source.hpp`,
`src/ui/style/theme_source_{parse,build}.cpp`, test `theme_source`
(`tests/unit/theme_source_tests.cpp`), fixture
`tests/fixtures/themes/spike-instrument.theme.json`.

## 1. Why a source format at all

ADR-064 item 6 asks for a theme authored as a source with `name`, `extends`, scale generators,
`tokens`, `components` and `adaptations`, resolved into one flat theme at build time, and emitted as
plain CSS custom properties inside `@layer`s. The reasons are all about what the output is:

- **One token name per role.** A colour role is one token whose value is a `light-dark()` pair, so
  there are never separate light and dark names. A component reads `--color-accent-fill` and the
  scheme decides the value.
- **Plain CSS.** The output is inspectable, diffable and needs no runtime. The cascade already
  knows how to resolve it, so a theme cannot drift from what the engine paints.
- **Layered.** Everything lands in `@layer tessera-ui.theme`, so unlayered app CSS outranks it
  regardless of specificity (ADR-063, ADR-064 item 6).

## 2. The source format

One JSON document. The spike implements four of the five members ADR-064 item 6 names; the fifth
(`adaptations`) is accepted and ignored, and the gap is recorded in section 6.

```json
{
  "name": "spike-instrument",
  "extends": "spike-base",
  "tokens": {
    "--color-accent-fill": ["#0b5cad", "#4c9be8"],
    "--space-control-x": "12px"
  },
  "components": {
    "Button": {
      "class": "tbtn",
      "keys": {
        "base": { "background-color": "var(--color-accent-fill)" },
        "variant:secondary": { "color": "var(--color-text-primary)" },
        "state:hover": { "background-color": "var(--color-background-sunken)" }
      }
    }
  }
}
```

| Member | Shape | Rule |
|---|---|---|
| `name` | string | `[a-z][a-z0-9-]*`; required |
| `extends` | string | the theme this one derives from; the chain must link up |
| `tokens` | object | a token name (starting `--`) to a string or a `[light, dark]` pair |
| `components` | object | a component name to `{ "class": <name>, "keys": { <key>: <declarations> } }` |
| `adaptations` | array | accepted and ignored by this spike (section 6) |

A token whose value is a **string** is scheme-independent: a length, a font stack, a number. It is
emitted once, as the bare value. A token whose value is a **two-element array** is a colour role: it
is emitted as one `light-dark(light, dark)` pair. The build decides which by parsing the value with
the engine's own `parse_color`, so the build cannot disagree with the paint path about whether a
value is a colour — and a `light-dark()` on a length, which the engine rejects, is never emitted.

## 3. Component keys

A key is one of a closed set of forms, and each compiles to a selector over the component's stable
class (ADR-064 item 5: a theme reaches a component through a stable class, never a generated one).

| Key | Selector for class `tbtn` |
|---|---|
| `base` | `.tbtn` |
| `variant:primary` | `.tbtn-primary` |
| `size:sm` | `.tbtn-sm` |
| `state:hover` | `.tbtn:hover` |
| `variant:primary+size:sm` | `.tbtn-primary.tbtn-sm` |
| `variant:primary+state:hover` | `.tbtn-primary:hover` |

A key outside those forms is `TESSERA_THEME_SOURCE_KEY`, not a silently dropped rule. The class
suffix form is deliberate: the Button sets `tbtn tbtn-primary`, so `variant:primary` is
`.tbtn-primary` and not `.tbtn.tbtn-primary` — the latter would also match a `tbtn` that is not
primary, which is a different rule.

## 4. `extends`

`extends` resolves into one flat theme at build time, so a shipped theme is never a graph. The build
takes an ordered chain (base first, derived last); each entry's `extends` must name the entry before
it, and the first must not extend anything. A chain that does not link up is
`TESSERA_THEME_SOURCE_EXTENDS` rather than a silent partial merge, because a theme that quietly lost
its base is a look nobody authored.

Later entries win, per declaration rather than per key: a derived theme can restate one property of
one key and inherit the rest. A token of the same name replaces the earlier one.

## 5. Diagnostics

| Code | When |
|---|---|
| `TESSERA_THEME_SOURCE_SHAPE` | Not an object, a wrong-typed member, a one-element pair, an unknown member |
| `TESSERA_THEME_SOURCE_NAME` | A theme or component name outside the pattern, or a missing `name` |
| `TESSERA_THEME_SOURCE_TOKEN` | A token name that does not start with `--` |
| `TESSERA_THEME_SOURCE_KEY` | A component key outside the closed forms |
| `TESSERA_THEME_SOURCE_EXTENDS` | The chain does not link up |
| `TESSERA_THEME_SOURCE_UNUSED_TOKEN` | Warning: the theme defines a token no component key reads |
| `TESSERA_THEME_SOURCE_UNKNOWN_TOKEN` | Warning: a component reads a token the theme does not define |

The last two are warnings, not errors: a theme may define a token for app CSS, and a component may
read a token the engine's own defaults supply. They are the vocabulary-cost measurement of section 7.

## 6. What this spike does not do

- **`adaptations`.** ADR-064 item 6 asks for ordered conditional overrides on width, pointer,
  contrast and reduced motion, compiled to media queries. The spike accepts the member and ignores
  it. The engine's media evaluation already handles `prefers-color-scheme`,
  `prefers-reduced-motion` and the viewport features, so the missing piece is the source-side
  syntax and the ordering rule, not the engine.
- **Scale generators.** ADR-064 item 6 names type, radius, motion and accent-colour generators. The
  spike writes every token explicitly, which is what the generator would emit anyway; the generator
  is authoring convenience, not a different output.
- **`prefers-contrast`.** The engine has no `prefers-contrast` media feature, so a high-contrast
  variant is a second theme id today (the shipped `instrument-high-contrast`), not a media query.
  The spike measures high contrast by resolving the same token set with the stricter floors.
- **A CLI.** The build is a library call. A `tessera theme build` subcommand is the natural next
  step and is not part of the spike.

## 7. The vocabulary cost

The build reports `read_tokens`: the token names the component keys actually read, in first-use
order. That is the measurement ADR-064 item 6 asks for — how many tokens a theme defines against how
many a control reads.

For the fixture (`tests/fixtures/themes/spike-instrument.theme.json`), Button reads 15 of the 20
tokens the theme defines. The five it does not read are the page surface, the muted text role, the
compact spacing, the compact control height and the focus ring's own value: they belong to other
components or to app CSS. That ratio is the argument for a token vocabulary that is defined per
role rather than per component, and it is the number a future token spec should keep in view.

## 8. Tests

| Test | What it proves |
|---|---|
| `theme_source` | The output shape (one `@layer`, one `:root`, one rule per key, no generated class names); a colour role is one `light-dark()` pair and a length is emitted once; `extends` flattens and overrides per declaration; the engine's own parser and cascade accept the output and resolve it in both schemes; the vocabulary cost is reported; every diagnostic fires; the key-to-selector mapping |
