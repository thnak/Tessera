// SPDX-License-Identifier: MIT

// The geometry contract of a validated skin (issue #171's skins slice;
// `designer-studio-v1.md` section 5.1 is normative).
//
// **The rule this module exists for.** A skin declares the OUTER device box and the insets; the page
// derives the viewer size as `size - bezel`. The skin must never change the engine's logical size
// behind its back: the size `viewerOf` returns is the size the page passes to `viewport.set`, and the
// insets `safeAreasOf` returns are the ones it passes to `tessera_canvas_set_safe_area`. There is no
// second derivation anywhere, which is what makes "the size in `viewport.set` is the size the preview
// renders at" checkable by reading one file.
//
// **The three modes of section 5.1** are a property of the pair (skin, project), not of the skin:
//
//   system   a desktop window whose title bar the OS draws. The canvas is the client area, so the
//            title strip sits OUTSIDE it and the viewer reports zero insets.
//   custom   a desktop window whose title bar the engine draws (`window.titleBar: "custom"`). The
//            skin draws only the bezel: the framework title bar is engine content, so the studio
//            shows it from the engine and hides the simulated one.
//   mobile   a phone, tablet or foldable. The canvas is the full display, the bars are overlaid, and
//            their heights are the insets the engine pads by.
//
// No DOM, no React: the page and the Node gate both call this.

import type { Skin, SkinInsets, SkinSize } from "./skins";

/** The three geometry modes of section 5.1. */
export type GeometryMode = "system" | "custom" | "mobile";

/**
 * The geometry mode of `skin` under a project's `window.titleBar`.
 *
 * `titleBar` is the project's `tessera.json` value (`system` or `custom`, section 5.1's "Desktop
 * `custom`" row); an absent or unknown value is `system`, which is the config's own default.
 */
export function geometryModeOf(skin: Skin, titleBar: string | null | undefined): GeometryMode {
  if (skin.category !== "desktop") {
    return "mobile";
  }
  return titleBar === "custom" ? "custom" : "system";
}

/**
 * The viewer rectangle: the size the page passes to `viewport.set`.
 *
 * It is the skin's derived `viewer` and nothing else. This function exists so the page has one name
 * for "the size the engine lays out", rather than reading `skin.viewer` in some places and
 * recomputing `size - bezel` in others.
 */
export function viewerOf(skin: Skin): SkinSize {
  return { width: skin.viewer.width, height: skin.viewer.height };
}

/**
 * The `safeAreas` a skin hands the viewer (section 5.1: "its insets are passed to the viewer as
 * `safeAreas` in `viewport.set`, exactly as `WindowMetrics::insets` would arrive from the HAL").
 *
 * A desktop skin reports zero insets whatever it declares, because section 5.1 says the desktop
 * canvas is the client area and the title bar is outside it. That is not a special case in the
 * manifest: it is the geometry contract, and it lives here so the page cannot get it wrong.
 */
export function safeAreasOf(skin: Skin): SkinInsets {
  return skin.category === "desktop"
    ? { top: 0, right: 0, bottom: 0, left: 0 }
    : { ...skin.insets };
}

/** A cutout rectangle in viewer coordinates. */
export interface CutoutRect {
  x: number;
  y: number;
  width: number;
  height: number;
}

/**
 * The cutout rectangle a skin hands the viewer, in viewer coordinates, or `null` for none.
 *
 * **The manifest's cutout coordinates are the display's**, not the outer frame's: section 5.2's own
 * example puts a punch-hole at `(206, 14)` on a device whose top bezel is 12 px, which is only
 * inside the screen if the coordinates are the screen's. A cutout is a display feature, so that is
 * also the reading that makes it a `WindowMetrics::cutouts` rectangle rather than a decoration.
 *
 * A `punch-hole` is the circle's bounding box. A `notch` hangs from the edge it touches, so only the
 * part inside the display is a rectangle: a top notch centred at `y = 0` occupies `0 .. y + radius`,
 * not `y - radius .. y + radius`, and reporting the negative half would put the rectangle outside
 * the viewer the engine lays out.
 *
 * A desktop skin reports none because its canvas is the client area and a notch is a display
 * feature.
 */
export function cutoutRectOf(skin: Skin): CutoutRect | null {
  if (skin.cutout.type === "none" || skin.category === "desktop") {
    return null;
  }
  const { x, y, radius } = skin.cutout;
  if (skin.cutout.type === "punch-hole") {
    return { x: x - radius, y: y - radius, width: radius * 2, height: radius * 2 };
  }
  // A notch: the half of the circle that is inside the display, on the edge it touches.
  if (y - radius <= 0) {
    return { x: x - radius, y: 0, width: radius * 2, height: y + radius };
  }
  if (y + radius >= skin.viewer.height) {
    return {
      x: x - radius,
      y: y - radius,
      width: radius * 2,
      height: skin.viewer.height - (y - radius),
    };
  }
  // A notch that touches no edge is not a notch; the validator accepts the shape, so the page draws
  // the circle it describes rather than inventing an edge for it.
  return { x: x - radius, y: y - radius, width: radius * 2, height: radius * 2 };
}

/**
 * The bars the page draws for `skin` in `mode`, in logical pixels, as the four edges they occupy.
 *
 * This is the one place the mode decides which bars exist, so the renderer and the check cannot
 * disagree about what a mode draws:
 *
 *   system   the title strip (and, on macOS, the menu bar) above the canvas; nothing overlaid.
 *   custom   nothing: the engine draws the framework title bar, and the skin adds only the bezel.
 *   mobile   the status bar overlaid at the top and the navigation bar at the bottom, inside the
 *            insets the engine already pads by.
 */
export interface SkinBars {
  /** The title strip's height above the canvas; 0 when the mode draws none. */
  title: number;
  /** The menu strip's height above the title strip; 0 when the mode draws none. */
  menu: number;
  /** The overlaid status bar's height; 0 when the mode draws none. */
  status: number;
  /** The overlaid navigation bar's height; 0 when the mode draws none. */
  navigation: number;
  /** Whether the navigation bar shows the gesture pill instead of three buttons. */
  gesture: boolean;
}

export function barsOf(skin: Skin, mode: GeometryMode): SkinBars {
  if (mode === "custom") {
    // The framework title bar is engine content (section 5.1), so the studio shows it from the
    // engine and hides the simulated one. Drawing both would show two title bars.
    return { title: 0, menu: 0, status: 0, navigation: 0, gesture: false };
  }
  if (mode === "mobile") {
    return {
      title: 0,
      menu: 0,
      status: skin.chrome.statusBar,
      navigation: skin.chrome.navigationBar,
      gesture: skin.chrome.gestureBar,
    };
  }
  return {
    title: skin.chrome.titleBar,
    menu: skin.chrome.menuBar,
    status: 0,
    navigation: 0,
    gesture: false,
  };
}

/**
 * The outer box the page lays out: the viewer plus the bezel plus the strips drawn outside it.
 *
 * The strips are OUTSIDE the canvas in `system` mode (section 5.1: "the canvas is the client area"),
 * so the frame the visitor sees is taller than the viewer by exactly the title and menu strips. In
 * `mobile` mode the bars are overlaid, so they add nothing.
 */
export function outerBoxOf(skin: Skin, mode: GeometryMode): SkinSize {
  const bars = barsOf(skin, mode);
  return {
    width: skin.viewer.width + skin.bezel.left + skin.bezel.right,
    height:
      skin.viewer.height +
      skin.bezel.top +
      skin.bezel.bottom +
      bars.title +
      bars.menu,
  };
}
