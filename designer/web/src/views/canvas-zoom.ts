// SPDX-License-Identifier: MIT

// The canvas pane's zoom arithmetic, kept pure so the gate can test it without a browser.
//
// **Zoom is a view of the preview, not an input to the app.** The engine keeps laying out at the device's
// logical viewport and the canvas element keeps its logical CSS size; the pane scales the whole stage (the
// canvas and its skin frame) with a CSS transform. Nothing here reaches the engine, so a zoom never costs a
// frame, never changes `viewport` and never restarts the app. The two places that turn a pointer position
// into logical coordinates (`canvas-pointer.ts`, `canvas-picker.ts`) read the zoom back with `cssZoomOf`.

/** The smallest and largest zoom the pane offers. */
export const MIN_ZOOM = 0.1;
export const MAX_ZOOM = 4;

/** The levels the zoom buttons step through, in order. */
export const ZOOM_STEPS: readonly number[] = [0.1, 0.25, 0.33, 0.5, 0.67, 0.75, 1, 1.25, 1.5, 2, 3, 4];

/** "Fit" follows the pane as it resizes; a scale is a level the person chose. */
export type ZoomMode = { kind: "fit" } | { kind: "scale"; value: number };

/** `value` held inside `[MIN_ZOOM, MAX_ZOOM]`; a non-finite value is 1. */
export function clampZoom(value: number): number {
  if (!Number.isFinite(value) || value <= 0) {
    return 1;
  }
  return Math.min(MAX_ZOOM, Math.max(MIN_ZOOM, value));
}

/**
 * The zoom that shows the whole stage in the available box. It never enlarges past 100%: "fit" is for a
 * stage that does not fit, and blowing a small preview up would only blur it. A stage or box with no size
 * yet (the pane is hidden) is 100%.
 */
export function fitZoom(
  available: { width: number; height: number },
  natural: { width: number; height: number },
): number {
  if (natural.width <= 0 || natural.height <= 0 || available.width <= 0 || available.height <= 0) {
    return 1;
  }
  return clampZoom(Math.min(1, available.width / natural.width, available.height / natural.height));
}

/** The next level above (`direction` 1) or below (`-1`) `current`; the end of the list stays put. */
export function stepZoom(current: number, direction: 1 | -1): number {
  const epsilon = 1e-6;
  if (direction === 1) {
    const next = ZOOM_STEPS.find((level) => level > current + epsilon);
    return next ?? ZOOM_STEPS[ZOOM_STEPS.length - 1];
  }
  for (let index = ZOOM_STEPS.length - 1; index >= 0; index -= 1) {
    if (ZOOM_STEPS[index] < current - epsilon) {
      return ZOOM_STEPS[index];
    }
  }
  return ZOOM_STEPS[0];
}

/** A zoom as the label a person reads: `100%`, `67%`. */
export function zoomLabel(zoom: number): string {
  return `${Math.round(zoom * 100)}%`;
}

/**
 * The scroll offset that keeps the point under the cursor still when the zoom changes from `before` to
 * `after`. `cursor` is the cursor's offset inside the scrolling box, `scroll` the box's current offset.
 */
export function anchoredScroll(scroll: number, cursor: number, before: number, after: number): number {
  if (before <= 0) {
    return scroll;
  }
  return Math.max(0, (scroll + cursor) * (after / before) - cursor);
}

/**
 * The scale a CSS transform put on `element` between its layout box and the screen. `getBoundingClientRect`
 * is the transformed size and `offsetWidth` is the layout one, so their ratio is the zoom; an element with
 * no layout width is unscaled.
 */
export function cssZoomOf(rectWidth: number, offsetWidth: number): number {
  return offsetWidth > 0 && rectWidth > 0 ? rectWidth / offsetWidth : 1;
}
