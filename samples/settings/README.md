# settings

The Settings starter template (issues #240 and #527, [component-library.md](../../docs/design/component-library.md)
section 8, stage 1): one screen a stranger can read, build and run. `SettingsPage.tsx` is built only from
public library components (`Switch`, `Checkbox`, `RadioGroup`/`Radio`, `ProgressBar`, `Details`, `Button`)
plus the intrinsic `View` and `Text`; `settings.css` uses theme tokens only, so every theme, density, text
scale and direction restyles it.

It has no `tessera.json` for the reason the gallery has none: it imports library components by relative path
(`./Switch`), and the build copies them next to it. The page is a gallery page, built and goldened by the
gallery harness:

```text
gallery --page settings [--theme instrument-dark] [--density compact] [--text-scale 150] [--rtl]
```

- Goldens: `tests/golden/gallery/settings_*.png` (three themes, compact, text scale 150, right to left), ctest
  `gallery_golden_settings_*`, lavapipe only.
- Interaction and codegen compile test: ctest `gallery_settings_runtime` builds the generated page through the
  codegen CLI and the C++ compiler, then drives it with the real `InputRouter` (pointer, Space, Enter).
- Web: the same page is `?page=settings` in the `gallery_web` target.

## Gaps the template exposed

What the page fakes with plain `View`s because the library component does not exist yet (V1 widget set,
[v1-widget-set.md](../../docs/design/v1-widget-set.md)): Heading, Text, Divider, Spacer, Card and Stack (here
`s-title`, `s-rule`, `s-spacer`, `s-card`, `s-section` classes), Select, TextInput inside Field, Badge,
Tooltip, and a navigation shell. Each is tracked as an issue.
