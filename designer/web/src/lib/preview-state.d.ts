// SPDX-License-Identifier: MIT

// Type declarations for `preview-state.js`, the studio's environment vocabulary (issue #169, P5).
//
// This file stays `.js` on purpose: it is a pure ES module with no DOM and no React, and the Node gates import
// it directly so they test the shipped source (ADR-054, Consequences).

/** The file name the studio compiles under. */
export const DEFAULT_FILE: string;

/** The viewport presets the toolbar offers: `[width, height, label]` in logical pixels. */
export const VIEWPORTS: ReadonlyArray<readonly [number, number, string]>;

/** The DPR values the toolbar offers. */
export const DPRS: readonly number[];

/** The theme ids the toolbar offers; `system` follows `prefers-color-scheme`. */
export const THEMES: readonly string[];

/**
 * The simulated fold postures the toolbar offers (issue #408): `""` (no fold), `flat`, `book`,
 * `tabletop`. The set and the names are the HAL's (`tessera/hal/fold_posture.hpp` parses them).
 */
export const FOLD_POSTURES: readonly string[];

/** The toolbar labels for `FOLD_POSTURES`, keyed by the value `set_viewport` carries. */
export const FOLD_POSTURE_LABELS: Readonly<Record<string, string>>;

/**
 * The bundled skins the toolbar offers (issue #171's skins slice): `""` (no frame) plus the
 * directory names under `designer/web/public/skins/`. The validated set is what the page offers; this
 * is the vocabulary, so the toolbar can render before any manifest loads.
 */
export const SKIN_IDS: readonly string[];

/**
 * The bundled device profiles (issue #447): `""` (none), the directory names under
 * `designer/web/public/profiles/`, and `custom`, the ad hoc profile a custom size makes.
 */
export const PROFILE_IDS: readonly string[];

/** `id -> [width, height, dpr]` of each bundled profile; what choosing it applies. */
export const PROFILE_GEOMETRY: Readonly<Record<string, readonly [number, number, number]>>;

/** The bounds of a custom size, `[min, max]` in logical pixels. */
export const CUSTOM_SIZE_RANGE: readonly [number, number];

/** `"390x844"` -> `{ width: 390, height: 844 }`; throws on anything the toolbar could not produce. */
export function parseViewport(text: string): { width: number; height: number };
