// SPDX-License-Identifier: MIT

// The preview's environment vocabulary: the viewport, DPR, theme, fold, skin and profile constants, with no
// DOM (issue #169, P5 of docs/design/reference-equantic-ui.md section 7.4). The toolbar constrains what a
// visitor can pick and the page renders from the same values; keeping the module pure lets the Node gates
// import the shipped source.

/** The file name the preview pages compile under; the default entry name. */
export const DEFAULT_FILE = 'ui/Preview.tsx';

/** The viewport presets the toolbar offers: `[width, height, label]` in logical pixels. */
export const VIEWPORTS = Object.freeze([
  [320, 240, '320x240 (P2 preview)'],
  [390, 844, '390x844 (phone)'],
  [768, 1024, '768x1024 (tablet)'],
  [1280, 800, '1280x800 (desktop)'],
]);

/** The DPR values the toolbar offers. */
export const DPRS = Object.freeze([1, 2, 3]);

/** The theme ids the toolbar offers: "system" follows `prefers-color-scheme`. */
export const THEMES = Object.freeze(['system', 'light', 'dark']);

/**
 * The simulated fold postures the toolbar offers (issue #408, `designer-studio-v1.md` section 6:
 * "Fold posture (foldables)" -> `viewport.set` `foldPosture`).
 *
 * The set is the HAL's (`tessera/hal/fold_posture.hpp` is the one parser): `flat` is a fully open
 * foldable, `book` a half-opened one with a vertical hinge, `tabletop` a half-opened one with a
 * horizontal hinge. `''` is "no fold at all", which is the desktop case and the default; it is not a
 * named posture because `viewport.set`'s rule is that an omitted field keeps its value.
 */
export const FOLD_POSTURES = Object.freeze(['', 'flat', 'book', 'tabletop']);

/** The posture labels the toolbar shows; the keys are the values `set_viewport` carries. */
export const FOLD_POSTURE_LABELS = Object.freeze({
  '': 'None',
  flat: 'Flat (unfolded)',
  book: 'Book (vertical hinge)',
  tabletop: 'Tabletop (horizontal hinge)',
});

/**
 * The bundled skins the toolbar offers (issue #171's skins slice, `designer-studio-v1.md` section
 * 5.2). The ids are the directory names under `designer/web/public/skins/`; `''` is "no frame",
 * which is the pre-#171 page and the default.
 *
 * The list is here rather than fetched because the toolbar must render before any manifest loads:
 * a `<select>` whose options arrive asynchronously would show an empty control on first paint. The
 * *validated* set is what the page offers (a rejected manifest is not offered), so this is the
 * vocabulary and `views/skin-load.ts` is the authority.
 */
export const SKIN_IDS = Object.freeze([
  '',
  'generic-desktop-windows',
  'generic-desktop-macos',
  'generic-desktop-linux',
  'generic-phone-android',
  'generic-phone-ios',
  'generic-tablet',
  'generic-foldable',
]);

/**
 * The bundled device profiles (issue #447, `studio-device-profiles-v1.md` section 2): `''` (none) plus
 * the directory names under `designer/web/public/profiles/`, and `custom`, the ad hoc profile a custom
 * size makes. Like `SKIN_IDS` this is the vocabulary the codec validates against; the validated set
 * the page offers is `views/profile-load.ts`'s.
 */
export const PROFILE_IDS = Object.freeze([
  '',
  'android-phone',
  'ios-phone',
  'android-tablet',
  'foldable',
  'desktop-windows',
  'desktop-macos',
  'desktop-linux',
  'custom',
]);

/**
 * The size and DPR each bundled profile applies: `id -> [width, height, dpr]`, which the device
 * controls apply when the profile is chosen.
 * `scripts/ci/preview_profiles.mjs` asserts every row against its `profile.json`.
 */
export const PROFILE_GEOMETRY = Object.freeze({
  'android-phone': Object.freeze([412, 915, 2.625]),
  'ios-phone': Object.freeze([390, 844, 3]),
  'android-tablet': Object.freeze([800, 1280, 2]),
  foldable: Object.freeze([884, 1104, 2.625]),
  'desktop-windows': Object.freeze([1280, 800, 1]),
  'desktop-macos': Object.freeze([1440, 900, 2]),
  'desktop-linux': Object.freeze([1280, 800, 1]),
});

/** The bounds of a custom size (the `custom` profile), in logical pixels. */
export const CUSTOM_SIZE_RANGE = Object.freeze([100, 4096]);

/** `"390x844"` -> `{ width: 390, height: 844 }`; throws on anything the toolbar could not produce. */
export function parseViewport(text) {
  const match = /^(\d+)x(\d+)$/.exec(String(text).trim());
  if (!match) {
    throw new Error(`not a viewport: ${text}`);
  }
  return { width: Number(match[1]), height: Number(match[2]) };
}
