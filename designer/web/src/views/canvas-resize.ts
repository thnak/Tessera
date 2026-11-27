// SPDX-License-Identifier: MIT

// The canvas pane's drag-resize arithmetic (issue #690, slice b), kept pure so the gate can test it
// without a browser.
//
// **A drag is a burst, and the contract is one frame per burst.** Every other change in the preview
// follows that rule (`canvas-frame.ts`, and `docs/design/canvas-input-session.md` #3); a resize drag is
// the one gesture that can produce a hundred of them, because a pointer produces a move per display
// refresh and each move is a real size. So the drag does not call `viewport.set` per move: it records
// the latest pointer position, paints the size it WOULD apply as a page-only readout, and asks for one
// coalesced step. The step is the unit that reaches the engine, and the last one runs on pointer-up so
// the settled size is exact even when the pointer stopped between animation frames.
//
// Nothing here touches the DOM or the engine: the caller supplies the projection (CSS px -> logical px,
// which is `cssZoomOf` in `canvas-zoom.ts`) and the limits (`VIEWPORT_LIMITS` in `canvas-viewport.ts`),
// so the arithmetic and the bounds are the ones the engine applies, not a second copy.

import { clampViewport, VIEWPORT_LIMITS, type Viewport } from "./canvas-viewport";

/** Which edge a handle drags. The corner drags both axes; an edge drags its own. */
export type ResizeAxis = "width" | "height" | "both";

/** One handle: its axis, and the side of the box it sits on (which picks the drag's sign). */
export type ResizeHandle = "right" | "bottom" | "corner";

/** The handle's axis and sign: dragging the right/bottom edge outward makes the box bigger. */
export function axisOf(handle: ResizeHandle): ResizeAxis {
  switch (handle) {
    case "right":
      return "width";
    case "bottom":
      return "height";
    case "corner":
      return "both";
  }
}

/**
 * The size a drag has reached: the start size plus the pointer's travel, divided by the pane's zoom.
 *
 * **The divisor is the zoom and NOT the DPR.** The session sizes the canvas' CSS box to the *logical*
 * viewport (`sizeToDevicePixels` sets `style.width` from the logical width), so one CSS pixel of the
 * canvas' box is one logical pixel however many device pixels it covers. The DPR only enters the
 * backing store. Dividing by it here halved every drag at DPR 2 - found by the browser gate, which is
 * the only place that combination is exercised.
 */
export function draggedSize(
  start: { width: number; height: number },
  handle: ResizeHandle,
  travel: { x: number; y: number },
  zoom: number,
): { width: number; height: number } {
  // A non-finite or non-positive zoom is 1, so a pane measured before layout cannot collapse a drag.
  const scale = Number.isFinite(zoom) && zoom > 0 ? zoom : 1;
  const axis = axisOf(handle);
  const grown = (length: number, delta: number): number =>
    Math.round(Math.max(VIEWPORT_LIMITS.minExtent, length + delta / scale));
  return {
    width: axis === "height" ? start.width : grown(start.width, travel.x),
    height: axis === "width" ? start.height : grown(start.height, travel.y),
  };
}

/**
 * The viewport a drag would apply, or `null` when the arithmetic produced something no engine call can
 * take (which `clampViewport` decides, so there is one rule).
 */
export function draggedViewport(
  start: { width: number; height: number; dpr: number },
  handle: ResizeHandle,
  travel: { x: number; y: number },
  zoom = 1,
): Viewport | null {
  const size = draggedSize(start, handle, travel, zoom);
  return clampViewport({ width: size.width, height: size.height, dpr: start.dpr });
}

/**
 * When a drag is allowed to reach the engine, so one gesture costs one frame per display refresh
 * rather than one per pointer event.
 *
 * Two inputs decide: the clock (a step no sooner than one animation frame after the last one) and
 * whether the size actually changed (a move that lands on the same rounded size costs nothing, which
 * is most of a slow drag). `settle` overrides the clock: pointer-up owes exactly one final step, so the
 * applied size is the one under the pointer rather than the last animation frame's.
 *
 * This is deliberately a decision function and not a timer: the caller owns `requestAnimationFrame`
 * (`canvas-session.ts` already coalesces frames with it), so there is still exactly one scheduler in
 * the page.
 */
export function shouldStep(
  previous: { size: { width: number; height: number } | null; atMs: number },
  next: { size: { width: number; height: number }; atMs: number },
  settle: boolean,
  frameMs = 16,
): boolean {
  if (settle) {
    // Pointer-up always applies, even onto the size already in force: the engine answers
    // `changed:false` for an unchanged size and costs no frame, so the guard cannot double a frame.
    return true;
  }
  if (previous.size === null) {
    return true;
  }
  if (next.size.width === previous.size.width && next.size.height === previous.size.height) {
    return false;
  }
  return next.atMs - previous.atMs >= frameMs;
}

/**
 * The readout a drag shows: the size it would apply and, when a limit reduced it, that it was clamped.
 *
 * It is drawn from the same `clampViewport` the engine call uses, so the number the person reads is the
 * number they will get - a readout computed separately would be free to disagree with the call.
 */
export function resizeReadout(viewport: Viewport | null): string {
  if (viewport === null) {
    return "no size";
  }
  const size = `${viewport.width}x${viewport.height}`;
  return viewport.clamped ? `${size} (clamped)` : size;
}
