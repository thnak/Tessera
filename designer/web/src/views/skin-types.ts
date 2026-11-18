// SPDX-License-Identifier: MIT

// The skin manifest's vocabulary (issue #171's skins slice; `designer-studio-v1.md` section 5.2 is
// normative for every field here).
//
// **Why the types are their own module.** The validator (`skins.ts`) is the half that decides what a
// manifest may say; this is the half that says what the words mean. Keeping them apart is what lets
// the renderer, the geometry module and the Node gate import the vocabulary without pulling in the
// rules, and it keeps both files inside the source-size limit.
//
// **Skins are data plus static art, never code.** One directory per skin under the site's `skins/`,
// each holding a `skin.json` and (optionally) an SVG frame. Nothing here evaluates a manifest.
//
// No DOM, no React: the page and the Node gate (`scripts/ci/preview_skins.mjs`) both call this.

/** The four categories of section 5.2; each selects a geometry mode of section 5.1. */
export type SkinCategory = "desktop" | "mobile" | "tablet" | "foldable";

/** The five operating systems of section 5.2; each selects a chrome variant. */
export type SkinOs = "windows" | "macos" | "linux" | "android" | "ios";

/** The three cutout shapes of section 5.2. `none` is the desktop answer. */
export type CutoutType = "none" | "notch" | "punch-hole";

export interface SkinSize {
  width: number;
  height: number;
}

export interface SkinInsets {
  top: number;
  right: number;
  bottom: number;
  left: number;
}

/** The physical frame drawn around the screen; `radius` is the outer corner radius. */
export interface SkinBezel extends SkinInsets {
  radius: number;
}

/**
 * A cutout, as a circle of `radius` centred at `(x, y)` in the display's own coordinates.
 *
 * The spec names exactly these four fields, so the module invents none: `type` selects how the page
 * draws it (a `punch-hole` is a circle, a `notch` is a tab hanging from the nearest edge down to
 * `y + radius` and spanning `x - radius .. x + radius`), and the geometry the validator checks is
 * the circle either way.
 */
export interface SkinCutout {
  type: CutoutType;
  x: number;
  y: number;
  radius: number;
}

/**
 * The simulated bars, in logical pixels. Every field is optional in a manifest and defaults by `os`
 * (section 5.2), so a skin that only wants a status bar declares only that.
 */
export interface SkinChrome {
  titleBar: number;
  menuBar: number;
  statusBar: number;
  navigationBar: number;
  gestureBar: boolean;
}

/** A validated skin: the manifest's fields plus the viewer rectangle the page derives from them. */
export interface Skin {
  /** The directory name under `skins/`; the picker's value and the manifest's identity. */
  id: string;
  schemaVersion: 1;
  name: string;
  category: SkinCategory;
  os: SkinOs;
  size: SkinSize;
  bezel: SkinBezel;
  cutout: SkinCutout;
  chrome: SkinChrome;
  insets: SkinInsets;
  /** The SVG frame's file name inside the skin's directory, or `null` for a generated frame. */
  art: string | null;
  /**
   * The viewer rectangle: `size` minus `bezel`. This is the size the page passes to `viewport.set`,
   * so the engine lays out exactly the rectangle the screen shows.
   */
  viewer: SkinSize;
}

/** A rejected skin. `code` is the spec's `TESSERA_STUDIO_SKIN`; `reason` is a sentence for a person. */
export interface SkinRejection {
  ok: false;
  code: "TESSERA_STUDIO_SKIN";
  reason: string;
}

export type SkinResult = { ok: true; skin: Skin } | SkinRejection;

/** The bundled V1 set of section 5.2, in the order the picker offers them. */
export const SKIN_IDS = Object.freeze([
  "generic-desktop-windows",
  "generic-desktop-macos",
  "generic-desktop-linux",
  "generic-phone-android",
  "generic-phone-ios",
  "generic-tablet",
  "generic-foldable",
]);

/** The skin a page starts from: the desktop window the pre-#171 page already previewed. */
export const DEFAULT_SKIN_ID = "generic-desktop-windows";

/** The categories the validator accepts, in the spec's order. */
export const SKIN_CATEGORIES: readonly SkinCategory[] = ["desktop", "mobile", "tablet", "foldable"];

/** The operating systems the validator accepts, in the spec's order. */
export const SKIN_OPERATING_SYSTEMS: readonly SkinOs[] = [
  "windows",
  "macos",
  "linux",
  "android",
  "ios",
];

/** The cutout shapes the validator accepts, in the spec's order. */
export const SKIN_CUTOUT_TYPES: readonly CutoutType[] = ["none", "notch", "punch-hole"];

/**
 * The bar sizes a manifest gets when it declares none, by `os` (section 5.2: "defaults by `os`").
 *
 * These are the sizes a generic window or phone of that platform shows, not a device's exact
 * metrics: the point of a bundled skin is a plausible frame, and a project that needs a specific
 * device declares its own numbers.
 */
export const CHROME_DEFAULTS: Readonly<Record<SkinOs, SkinChrome>> = Object.freeze({
  windows: { titleBar: 32, menuBar: 0, statusBar: 0, navigationBar: 0, gestureBar: false },
  macos: { titleBar: 28, menuBar: 24, statusBar: 0, navigationBar: 0, gestureBar: false },
  linux: { titleBar: 30, menuBar: 0, statusBar: 0, navigationBar: 0, gestureBar: false },
  android: { titleBar: 0, menuBar: 0, statusBar: 24, navigationBar: 24, gestureBar: true },
  ios: { titleBar: 0, menuBar: 0, statusBar: 24, navigationBar: 20, gestureBar: true },
});
