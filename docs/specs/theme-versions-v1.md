# Theme versions and pins v1

Status: normative for `ui/style` and `tools/codegen`. Decision:
[ADR-047](../../decisions/ADR-047-component-library-layers-and-design-language.md) item 4 (themes are
versioned separately from components and applications pin a theme version). The theme asset itself is
[`stylesheet-v1.md`](stylesheet-v1.md) section 9.1; its embedded form is
[`compiled-stylesheet-v1.md`](compiled-stylesheet-v1.md) section 11.

## 1. Theme version

The theme asset has an optional top-level `themeVersions` object: theme id to a version string.

- A version is `MAJOR.MINOR.PATCH`: three decimal components, no leading zeros, each at most nine
  digits, no pre-release or build suffix. Anything else is `TESSERA_THEME_VERSION`.
- An id that is neither `default` nor a key of `themes` is `TESSERA_THEME_ID`.
- A theme not listed is **unversioned**. The version is not a token: it is never injected into the
  `:root` rule and cannot be read with `var()`.
- The version is a property of the theme, separate from the asset `schemaVersion` and from any
  component release. Within one major version a theme's default visuals change only to fix bugs, each
  with reviewed goldens; a new look is a new major version or a new theme (ADR-047 item 4).

## 2. Pins

The project config `assets.themePins` ([`project-config-v1.md`](project-config-v1.md)) maps a theme id
to a pin: `MAJOR`, `MAJOR.MINOR` or `MAJOR.MINOR.PATCH` in the same decimal form.

- A theme satisfies a pin when the pin is a component-wise prefix of its version: pin `1` accepts
  `1.4.2` and not `10.0.0`; pin `1.4` accepts `1.4.2` and not `1.5.0`; pin `1.4.2` accepts only
  `1.4.2`.
- A malformed pin is a configuration error when the config is read.
- Pins are checked when the theme asset is embedded. A pin that names a theme the asset does not
  define, an unversioned theme, or a version the pin does not accept is the build error
  `TESSERA_THEME_VERSION_PIN`. The message names the theme and both versions. The first violation in
  config order is reported, no theme header is written, and the run exits 1.
- An app with no `themePins` entry is unpinned and accepts any version.

## 3. Embedded form

The blob carries each theme's version string (empty when unversioned) from blob format 2 on, so a
running app can read the version of the theme it loaded. Pins are a build-time check only and are not
stored.

## 4. Evidence

- `tests/unit/theme_version_tests.cpp`: version and pin shapes, the prefix rule, `themeVersions`
  parse errors, `check_theme_pins`, the blob round trip, and that the version is never injected.
- `tests/unit/codegen_theme_embed_tests.cpp` (`test_cli_theme_pins`): a satisfied pin embeds, an
  unsatisfied pin exits 1 with `TESSERA_THEME_VERSION_PIN`, a malformed pin exits 1.
- `tests/unit/instrument_theme_tests.cpp`: the shipped Instrument theme is version `1.0.0` in all
  three themes and satisfies pin `1`.
- Implementation: `include/tessera/ui/style/theme_asset.hpp` (`is_theme_version`,
  `theme_version_satisfies_pin`, `check_theme_pins`), `src/ui/style/theme_asset.cpp`,
  `tools/codegen/project_identity.cpp`, `tools/codegen/regenerate.cpp`.
