// SPDX-License-Identifier: MIT

// The preview's environment state: constants and validation, with no DOM and no compression (issue
// #169, P5 of docs/design/reference-equantic-ui.md section 7.4).
//
// It is separate from `share.js` (the codec) and `studio.js` (the toolbar and gallery) because all
// three need the same vocabulary and none of them owns it: the codec validates what it decodes, the
// toolbar constrains what a visitor can pick, and the page renders from the same values. Keeping it
// pure means the round-trip test imports the codec without dragging in DOM code.

/** The file name the preview pages compile under; a share link omits it while it stays the default. */
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

/** The state a page starts from; each field is also the decoder's fallback for a missing one. */
export const DEFAULT_STATE = Object.freeze({
  source: '',
  file: DEFAULT_FILE,
  width: 320,
  height: 240,
  dpr: 1,
  theme: 'system',
  foldPosture: '',
});

/**
 * The document caps, applied to the *decoded* state. `MAX_SOURCE_BYTES` is the per-file limit of
 * jsx-subset-v1.md section 15, and the envelope cap adds room for the other fields.
 */
export const MAX_SOURCE_BYTES = 1 << 20;
export const MAX_ENVELOPE_BYTES = MAX_SOURCE_BYTES + 4096;

/** A refusal the page can branch on by `code`, rather than by matching a message. */
export class ShareError extends Error {
  /**
   * @param {string} code `too-large` | `malformed` | `version` | `invalid` | `unsupported`
   * @param {string} message a sentence for a person, never a bare code
   */
  constructor(code, message) {
    super(message);
    this.name = 'ShareError';
    this.code = code;
  }
}

const encoder = new TextEncoder();

function positiveInteger(value) {
  return typeof value === 'number' && Number.isInteger(value) && value > 0;
}

/**
 * Validates and normalizes a state object. Every field is optional and falls back to DEFAULT_STATE,
 * except that an explicitly *invalid* value is an error: a page that recorded 0 DPR or a 3000-pixel
 * "phone" has a bug, and silently substituting a default would hide it in the one place (a shared
 * link) where the mistake is hardest to trace back.
 *
 * `viewportAllowed(width, height)` lets a caller accept a size the toolbar does not offer; the codec
 * passes the toolbar's list so a link can only carry what the page can show.
 */
export function normalizeState(state = {}, options = {}) {
  const viewportAllowed = options.viewportAllowed || allowedViewport;
  const merged = { ...DEFAULT_STATE, ...state };
  if (typeof merged.source !== 'string') {
    throw new ShareError('invalid', '`source` must be a string');
  }
  if (encoder.encode(merged.source).length > MAX_ENVELOPE_BYTES) {
    throw new ShareError('too-large', `the source is larger than ${MAX_ENVELOPE_BYTES} bytes`);
  }
  if (typeof merged.file !== 'string' || merged.file === '') {
    throw new ShareError('invalid', '`file` must be a non-empty string');
  }
  if (!positiveInteger(merged.width) || !positiveInteger(merged.height)) {
    throw new ShareError('invalid', 'the viewport must be positive integers');
  }
  if (!viewportAllowed(merged.width, merged.height)) {
    const allowed = VIEWPORTS.map(([width, height]) => `${width}x${height}`).join(', ');
    throw new ShareError('invalid', `the viewport must be one of ${allowed}`);
  }
  if (!DPRS.includes(merged.dpr)) {
    throw new ShareError('invalid', `the DPR must be one of ${DPRS.join(', ')}`);
  }
  if (!THEMES.includes(merged.theme)) {
    throw new ShareError('invalid', `the theme must be one of ${THEMES.join(', ')}`);
  }
  if (!FOLD_POSTURES.includes(merged.foldPosture)) {
    throw new ShareError('invalid', `the fold posture must be one of ${FOLD_POSTURES.join(', ')}`);
  }
  return {
    source: merged.source,
    file: merged.file,
    width: merged.width,
    height: merged.height,
    dpr: merged.dpr,
    theme: merged.theme,
    foldPosture: merged.foldPosture,
  };
}

function allowedViewport(width, height) {
  return VIEWPORTS.some(([w, h]) => w === width && h === height);
}

/** `"390x844"` -> `{ width: 390, height: 844 }`; throws on anything the toolbar could not produce. */
export function parseViewport(text) {
  const match = /^(\d+)x(\d+)$/.exec(String(text).trim());
  if (!match) {
    throw new ShareError('invalid', `not a viewport: ${text}`);
  }
  return { width: Number(match[1]), height: Number(match[2]) };
}
