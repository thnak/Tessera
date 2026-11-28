// SPDX-License-Identifier: MIT

// WCAG 2.x contrast for the studio's theme editor (issue #582). A plain module so the page and the Node gate
// import the same bytes. The formula and the role table mirror the C++ checker (`contrast.hpp`,
// `instrument_contrast_pairs()`, docs/specs/theme-generation-v1.md section 4): one table, so the editor
// cannot bless a pair the generator and `tessera theme --check` would refuse.

/** Variants that use the stricter high-contrast floors. */
const HIGH_CONTRAST = /high-contrast/;

const SURFACES = ['--surface', '--surface-raised', '--surface-sunken'];

/**
 * [foreground, background, role] for every pair the spec checks. `role` picks the floor: "text" 4.5 (7 in high
 * contrast), "large" and "nontext" 3 (4.5 in high contrast).
 */
export const CONTRAST_PAIRS = Object.freeze([
  ...SURFACES.flatMap((surface) => [
    ['--on-surface', surface, 'text'],
    ['--on-surface-muted', surface, 'text'],
    ['--accent', surface, 'large'],
    ['--focus-ring', surface, 'nontext'],
    ['--border-strong', surface, 'nontext'],
    ['--danger', surface, 'nontext'],
    ['--success', surface, 'nontext'],
    ['--warning', surface, 'nontext'],
  ]),
  ['--on-accent', '--accent', 'text'],
  ['--on-danger', '--danger', 'text'],
  ['--on-success', '--success', 'text'],
  ['--on-warning', '--warning', 'text'],
  ['--window-foreground', '--window-background', 'text'],
]);

/** The minimum ratio a role must clear in a variant. */
export function floorFor(role, variantId) {
  const high = HIGH_CONTRAST.test(variantId);
  if (role === 'text') {
    return high ? 7 : 4.5;
  }
  return high ? 4.5 : 3;
}

/**
 * Parses #rgb, #rgba, #rrggbb and #rrggbbaa into 0-255 channels plus alpha 0-1, or null for anything else
 * (a named color, rgb(), var(): the editor then reports the pair as unchecked rather than guessing).
 */
export function parseHex(value) {
  const match = /^#([0-9a-f]{3,4}|[0-9a-f]{6}|[0-9a-f]{8})$/i.exec(String(value).trim());
  if (match === null) {
    return null;
  }
  let digits = match[1];
  if (digits.length <= 4) {
    digits = [...digits].map((digit) => digit + digit).join('');
  }
  const channel = (index) => parseInt(digits.slice(index * 2, index * 2 + 2), 16);
  return { r: channel(0), g: channel(1), b: channel(2), a: digits.length === 8 ? channel(3) / 255 : 1 };
}

/** WCAG relative luminance of an opaque color. */
export function luminance({ r, g, b }) {
  const lin = (value) => {
    const unit = value / 255;
    return unit <= 0.04045 ? unit / 12.92 : ((unit + 0.055) / 1.055) ** 2.4;
  };
  return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

/** `front` composited over the opaque `back`, so a translucent text color is judged as it paints. */
function over(front, back) {
  const mix = (f, b) => Math.round(f * front.a + b * (1 - front.a));
  return { r: mix(front.r, back.r), g: mix(front.g, back.g), b: mix(front.b, back.b), a: 1 };
}

/** The WCAG contrast ratio (1 to 21) of two color strings, or null when either is not a hex color. */
export function contrastRatio(foreground, background) {
  const fg = parseHex(foreground);
  const bg = parseHex(background);
  if (fg === null || bg === null) {
    return null;
  }
  const solidBack = over(bg, { r: 255, g: 255, b: 255 });
  const [hi, lo] = [luminance(over(fg, solidBack)), luminance(solidBack)].sort((a, b) => b - a);
  return (hi + 0.05) / (lo + 0.05);
}

/**
 * Checks one variant. `tokens` maps a token name to its value. A pair is reported only when both tokens are
 * defined; a defined token that is not a hex color is reported with `ratio: null` and `pass: null`.
 *
 * @param {Record<string, string>} tokens
 * @param {string} variantId
 * @returns {{ foreground: string, background: string, role: string, floor: number, ratio: number | null, pass: boolean | null }[]}
 */
export function checkVariant(tokens, variantId) {
  const results = [];
  for (const [foreground, background, role] of CONTRAST_PAIRS) {
    if (tokens[foreground] === undefined || tokens[background] === undefined) {
      continue;
    }
    const floor = floorFor(role, variantId);
    const ratio = contrastRatio(tokens[foreground], tokens[background]);
    results.push({ foreground, background, role, floor, ratio, pass: ratio === null ? null : ratio >= floor });
  }
  return results;
}
