// SPDX-License-Identifier: MIT

// The skin manifest's validator (issue #171's skins slice; `designer-studio-v1.md` section 5.2 is
// normative for every rule here).
//
// **Skins are data plus static art, never code.** One directory per skin under the site's `skins/`,
// each holding a `skin.json` and (optionally) an SVG frame. Nothing in this module evaluates a
// manifest: it reads fields and refuses the ones the spec refuses. That is what lets a manifest be
// reviewed in a diff and shipped as a static file.
//
// **The result is a value, not a throw.** A rejected skin is an ordinary outcome - the spec says it
// "is not offered" - so `parseSkin` returns `{ok:false, code:"TESSERA_STUDIO_SKIN", reason}` and the
// caller decides whether to show a banner or fall back to the next skin. A throw would make a bad
// manifest a page crash in the one place (a picker) where the visitor can do nothing about it.
//
// **Chrome colours are not part of the skin** (section 5.2). A manifest that carries one is a bug,
// and `parseSkin` refuses it rather than ignoring it: an ignored colour would ship as a frame that
// never retints, which is the failure the rule exists to prevent. The page tints the bars from the
// theme's `{background, foreground, scheme}` record instead, so a theme switch retints the frame and
// the canvas in the same frame.
//
// The vocabulary is in `skin-types.ts`, the geometry the page derives is in `skin-geometry.ts`, and
// the fetch is in `skin-load.ts`: four responsibilities, four files, so each stays inside the
// source-size limit and a reader can find the rule they need.
//
// No DOM, no React: the page and the Node gate (`scripts/ci/preview_skins.mjs`) both call this.

import {
  CHROME_DEFAULTS,
  SKIN_CATEGORIES,
  SKIN_CUTOUT_TYPES,
  SKIN_OPERATING_SYSTEMS,
  type CutoutType,
  type SkinBezel,
  type SkinCategory,
  type SkinChrome,
  type SkinCutout,
  type SkinInsets,
  type SkinOs,
  type SkinRejection,
  type SkinResult,
  type SkinSize,
} from "./skin-types";

export * from "./skin-types";

function reject(reason: string): SkinRejection {
  return { ok: false, code: "TESSERA_STUDIO_SKIN", reason };
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

/** A finite, non-negative number, or `null`. `NaN` and `Infinity` are not geometry. */
function nonNegative(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) && value >= 0 ? value : null;
}

/** A finite, strictly positive number, or `null`. A zero-sized screen is not a screen. */
function positive(value: unknown): number | null {
  return typeof value === "number" && Number.isFinite(value) && value > 0 ? value : null;
}

/** One `{top, right, bottom, left}` block, or the reason it is not one. */
function readInsets(
  value: unknown,
  field: string,
): { ok: true; insets: SkinInsets } | SkinRejection {
  if (!isObject(value)) {
    return reject(`${field} must be an object with top, right, bottom and left`);
  }
  const top = nonNegative(value.top);
  const right = nonNegative(value.right);
  const bottom = nonNegative(value.bottom);
  const left = nonNegative(value.left);
  if (top === null || right === null || bottom === null || left === null) {
    return reject(`${field} must have four non-negative numbers (top, right, bottom, left)`);
  }
  return { ok: true, insets: { top, right, bottom, left } };
}

/**
 * The colour fields a manifest must not carry (section 5.2: "Chrome colours are not part of the
 * skin ... A skin that hard-codes a colour is a bug").
 *
 * The check is by name rather than by value because a colour can be spelled a dozen ways and a
 * value-shaped test would miss the next one. A manifest that names a colour anywhere is refused,
 * which is the only reading of "is a bug" that a validator can enforce.
 */
const COLOUR_KEYS = Object.freeze([
  "color",
  "colour",
  "colors",
  "colours",
  "background",
  "foreground",
  "bg",
  "fg",
  "fill",
  "stroke",
  "tint",
  "scheme",
]);

/** The first colour-shaped key anywhere in `value`, as a dotted path, or `null`. */
function findColourKey(value: unknown, path = ""): string | null {
  if (Array.isArray(value)) {
    for (let index = 0; index < value.length; index += 1) {
      const found = findColourKey(value[index], `${path}[${index}]`);
      if (found !== null) {
        return found;
      }
    }
    return null;
  }
  if (!isObject(value)) {
    return null;
  }
  for (const key of Object.keys(value)) {
    const here = path === "" ? key : `${path}.${key}`;
    if (COLOUR_KEYS.includes(key.toLowerCase())) {
      return here;
    }
    const found = findColourKey(value[key], here);
    if (found !== null) {
      return found;
    }
  }
  return null;
}

/** The bezel block, or the reason it is not one. Absent means "no frame", which is the desktop case. */
function readBezel(value: unknown): { ok: true; bezel: SkinBezel } | SkinRejection {
  if (value === undefined) {
    return { ok: true, bezel: { top: 0, right: 0, bottom: 0, left: 0, radius: 0 } };
  }
  const read = readInsets(value, "bezel");
  if (!read.ok) {
    return read;
  }
  const radius = nonNegative((value as Record<string, unknown>).radius);
  if (radius === null) {
    return reject("bezel.radius must be a non-negative number");
  }
  return { ok: true, bezel: { ...read.insets, radius } };
}

/** The cutout block, or the reason it is not one. Absent means "no cutout". */
function readCutout(value: unknown): { ok: true; cutout: SkinCutout } | SkinRejection {
  if (value === undefined) {
    return { ok: true, cutout: { type: "none", x: 0, y: 0, radius: 0 } };
  }
  if (!isObject(value)) {
    return reject("cutout must be an object with type, x, y and radius");
  }
  const type = value.type;
  if (typeof type !== "string" || !SKIN_CUTOUT_TYPES.includes(type as CutoutType)) {
    return reject(`cutout.type must be one of ${SKIN_CUTOUT_TYPES.join(", ")}`);
  }
  const x = nonNegative(value.x);
  const y = nonNegative(value.y);
  const radius = nonNegative(value.radius);
  if (x === null || y === null || radius === null) {
    return reject("cutout must have non-negative x, y and radius");
  }
  if (type !== "none" && radius <= 0) {
    return reject(`a ${type} cutout needs a positive radius`);
  }
  return { ok: true, cutout: { type: type as CutoutType, x, y, radius } };
}

/** The chrome block, defaulted field by field from `os`, or the reason it is not one. */
function readChrome(
  value: unknown,
  os: SkinOs,
): { ok: true; chrome: SkinChrome } | SkinRejection {
  const defaults = CHROME_DEFAULTS[os];
  if (value === undefined) {
    return { ok: true, chrome: { ...defaults } };
  }
  if (!isObject(value)) {
    return reject("chrome must be an object of bar sizes");
  }
  const bar = (name: "titleBar" | "menuBar" | "statusBar" | "navigationBar"): number | SkinRejection => {
    if (value[name] === undefined) {
      return defaults[name];
    }
    const read = nonNegative(value[name]);
    return read === null ? reject(`chrome.${name} must be a non-negative number`) : read;
  };
  const titleBar = bar("titleBar");
  const menuBar = bar("menuBar");
  const statusBar = bar("statusBar");
  const navigationBar = bar("navigationBar");
  // Each bar is either a number or the rejection that explains it, and the first rejection is the
  // answer: a manifest with two bad bars should name the first one rather than a generic failure.
  for (const read of [titleBar, menuBar, statusBar, navigationBar]) {
    if (typeof read !== "number") {
      return read;
    }
  }
  // A gesture bar is a shape, not a size: it is drawn as a pill inside the navigation inset, so it
  // adds no height of its own and only a manifest that asks for it gets one.
  const gestureBar = value.gestureBar === undefined ? defaults.gestureBar : value.gestureBar === true;
  return {
    ok: true,
    chrome: {
      titleBar: titleBar as number,
      menuBar: menuBar as number,
      statusBar: statusBar as number,
      navigationBar: navigationBar as number,
      gestureBar,
    },
  };
}

/**
 * The mobile rule of section 5.2: "insets ... must contain the cutout and bars for mobile".
 *
 * A phone whose status bar is taller than its top inset would draw the bar over content the engine
 * believes is clear, which is the same class of bug as a colour: the picture and the layout
 * disagree. A desktop skin is exempt because its canvas is the client area and its bars sit outside
 * it (section 5.1).
 */
function checkMobileInsets(
  category: SkinCategory,
  chrome: SkinChrome,
  insets: SkinInsets,
  cutout: SkinCutout,
  viewer: SkinSize,
): SkinRejection | null {
  if (category === "desktop") {
    return null;
  }
  if (insets.top < chrome.statusBar) {
    return reject(
      `insets.top (${insets.top}) is smaller than the ${chrome.statusBar} px status bar it must ` +
        "contain",
    );
  }
  if (insets.bottom < chrome.navigationBar) {
    return reject(
      `insets.bottom (${insets.bottom}) is smaller than the ${chrome.navigationBar} px ` +
        "navigation bar it must contain",
    );
  }
  if (cutout.type === "none") {
    return null;
  }
  // The cutout hangs from the edge it touches: a top cutout reaches down to `y + radius`, a bottom
  // one up to `y - radius`. Either must sit inside the inset band that reserves room for it. The
  // coordinates are the display's (section 5.2's own example puts a punch-hole at y=14 inside a
  // 12 px bezel), so no bezel arithmetic belongs here.
  const reachesTop = cutout.y - cutout.radius <= 0;
  const reachesBottom = cutout.y + cutout.radius >= viewer.height;
  if (reachesTop && cutout.y + cutout.radius > insets.top) {
    return reject(
      `the cutout reaches ${cutout.y + cutout.radius} px down but insets.top is ${insets.top}; ` +
        "the inset must contain the cutout",
    );
  }
  if (reachesBottom && viewer.height - (cutout.y - cutout.radius) > insets.bottom) {
    return reject(
      `the cutout reaches ${viewer.height - (cutout.y - cutout.radius)} px up but ` +
        `insets.bottom is ${insets.bottom}; the inset must contain the cutout`,
    );
  }
  return null;
}

/**
 * Validates one manifest and derives the viewer rectangle.
 *
 * `id` is the skin's directory name, which the manifest itself does not carry: the directory is the
 * identity, so a manifest cannot claim to be a skin it is not.
 *
 * Unknown fields are ignored (section 5.2), with the one exception of a colour, which is refused
 * rather than ignored because ignoring it would ship a frame that never retints.
 */
export function parseSkin(id: string, document: unknown): SkinResult {
  if (!isObject(document)) {
    return reject("the manifest is not a JSON object");
  }
  // The version is checked before anything else: a manifest from a future revision may mean
  // something different by a field this module reads, and guessing is how a preview shows a device
  // nobody asked for. A missing version is read as 1, because 1 is the only revision that exists.
  const version = document.schemaVersion === undefined ? 1 : document.schemaVersion;
  if (version !== 1) {
    return reject(`schemaVersion is ${JSON.stringify(version)}; this page reads 1`);
  }

  const colour = findColourKey(document);
  if (colour !== null) {
    return reject(
      `${colour} is a colour; chrome colours come from the theme, not the skin ` +
        "(designer-studio-v1.md section 5.2)",
    );
  }

  const category = document.category;
  if (typeof category !== "string" || !SKIN_CATEGORIES.includes(category as SkinCategory)) {
    return reject(`category must be one of ${SKIN_CATEGORIES.join(", ")}`);
  }
  const os = document.os;
  if (typeof os !== "string" || !SKIN_OPERATING_SYSTEMS.includes(os as SkinOs)) {
    return reject(`os must be one of ${SKIN_OPERATING_SYSTEMS.join(", ")}`);
  }

  if (!isObject(document.size)) {
    return reject("size must be an object with width and height");
  }
  const width = positive(document.size.width);
  const height = positive(document.size.height);
  if (width === null || height === null) {
    return reject("size must have a positive width and height");
  }

  const bezelRead = readBezel(document.bezel);
  if (!bezelRead.ok) {
    return bezelRead;
  }
  const bezel = bezelRead.bezel;

  const insetsRead = readInsets(document.insets, "insets");
  if (!insetsRead.ok) {
    return insetsRead;
  }
  const insets = insetsRead.insets;

  // The viewer rectangle, derived once. A bezel that eats the whole screen is not a device, and a
  // negative rectangle would be handed to `viewport.set` as a size the engine cannot lay out.
  const viewer = {
    width: width - bezel.left - bezel.right,
    height: height - bezel.top - bezel.bottom,
  };
  if (viewer.width <= 0 || viewer.height <= 0) {
    return reject(
      `the bezel (${bezel.left}+${bezel.right} x ${bezel.top}+${bezel.bottom}) leaves no screen ` +
        `inside a ${width}x${height} device`,
    );
  }

  // "an invalid geometry (inset larger than the screen ...) rejects the skin" (section 5.2). The
  // sum is what matters: two 300 px insets on a 500 px screen are each smaller than the screen and
  // still leave nothing to lay out.
  if (insets.left + insets.right >= viewer.width) {
    return reject(
      `the horizontal insets (${insets.left}+${insets.right}) leave no width inside a ` +
        `${viewer.width} px screen`,
    );
  }
  if (insets.top + insets.bottom >= viewer.height) {
    return reject(
      `the vertical insets (${insets.top}+${insets.bottom}) leave no height inside a ` +
        `${viewer.height} px screen`,
    );
  }

  const cutoutRead = readCutout(document.cutout);
  if (!cutoutRead.ok) {
    return cutoutRead;
  }
  const cutout = cutoutRead.cutout;

  const chromeRead = readChrome(document.chrome, os as SkinOs);
  if (!chromeRead.ok) {
    return chromeRead;
  }
  const chrome = chromeRead.chrome;

  const mobileProblem = checkMobileInsets(
    category as SkinCategory,
    chrome,
    insets,
    cutout,
    viewer,
  );
  if (mobileProblem !== null) {
    return mobileProblem;
  }

  const name = typeof document.name === "string" && document.name !== "" ? document.name : id;
  const art = typeof document.art === "string" && document.art !== "" ? document.art : null;
  if (art !== null && (art.includes("/") || art.includes("\\") || art.includes(".."))) {
    // The art is one file inside the skin's own directory. A path is a path the site would fetch,
    // and a manifest is data: it may not name a file outside its own skin.
    return reject(`art must be a file name inside the skin's directory, not a path (${art})`);
  }

  return {
    ok: true,
    skin: {
      id,
      schemaVersion: 1,
      name,
      category: category as SkinCategory,
      os: os as SkinOs,
      size: { width, height },
      bezel,
      cutout,
      chrome,
      insets,
      art,
      viewer,
    },
  };
}
