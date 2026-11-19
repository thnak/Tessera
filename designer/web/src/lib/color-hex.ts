// SPDX-License-Identifier: MIT

// The inspector's colour control speaks `#rrggbb` (an `<input type="color">` accepts nothing else), while a
// computed style value may be spelled `#rgb`, `#rrggbb` or `rgb(r, g, b)`. This reads those three into the
// control's form and returns `null` for everything else (named colours, `rgba()` with alpha, `var()`), for
// which the inspector offers the text field only: guessing a colour from a value it cannot read would put a
// wrong swatch beside the value.

const SHORT = /^#([0-9a-f])([0-9a-f])([0-9a-f])$/i;
const LONG = /^#[0-9a-f]{6}$/i;
const RGB = /^rgb\(\s*(\d{1,3})\s*[, ]\s*(\d{1,3})\s*[, ]\s*(\d{1,3})\s*\)$/i;

/** `#rrggbb` (lower case) for a colour value the control can show, else `null`. */
export function hexOf(value: string): string | null {
  const text = value.trim();
  const short = SHORT.exec(text);
  if (short !== null) {
    return `#${short[1]}${short[1]}${short[2]}${short[2]}${short[3]}${short[3]}`.toLowerCase();
  }
  if (LONG.test(text)) {
    return text.toLowerCase();
  }
  const rgb = RGB.exec(text);
  if (rgb !== null) {
    const parts = [rgb[1], rgb[2], rgb[3]].map(Number);
    if (parts.every((part) => part <= 255)) {
      return `#${parts.map((part) => part.toString(16).padStart(2, "0")).join("")}`;
    }
  }
  return null;
}

/** Whether `property` takes a colour, so the inspector shows a swatch for it. */
export function isColorProperty(property: string): boolean {
  return /(^|-)color$/.test(property) || property === "background" || property === "fill" || property === "stroke";
}
