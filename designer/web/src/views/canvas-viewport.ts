// SPDX-License-Identifier: MIT

// The Canvas viewport limits (issue #690), the page's half of one rule.
//
// **Why the page needs its own copy.** The page sizes the canvas backing store itself
// (`sizeToDevicePixels` sets `canvas.width`/`canvas.height` from extent x DPR), so a size the engine
// would clamp still reaches the browser unclamped if only the module knew the rule - and the browser
// allocates that surface before the module ever sees the numbers. The module clamps too
// (`tools/codegen/canvas_viewport_request.cpp`); the two copies are kept in step by
// `scripts/ci/preview_viewport_limits.mjs` and `tests/unit/canvas_viewport_limit_tests.cpp`, which
// assert the same numbers against the same table.
//
// The numbers mirror the engine's exactly: extent 1..8192, DPR 0.01..4, and a 256 MiB ceiling on
// extent x DPR x 4 bytes. A value over a limit is clamped and reported (so the pane can name the
// device it got); a value that names no size (0, negative, NaN) is refused, and the caller falls back
// to the last state the pane was in rather than asking the engine something it cannot answer.

/** The logical viewport the preview lays out at when the toolbar has not moved. */
export const VIEWPORT_WIDTH = 320;
export const VIEWPORT_HEIGHT = 240;

/** The limits every canvas viewport request is held to (issue #690); mirrors the engine. */
export const VIEWPORT_LIMITS = {
  minExtent: 1,
  maxExtent: 8192,
  minDpr: 0.01,
  maxDpr: 4,
  maxBackingBytes: 256 * 1024 * 1024,
} as const;

/** One legal viewport, and whether the request that produced it had to be reduced to fit. */
export interface Viewport {
  width: number;
  height: number;
  dpr: number;
  /** True when a limit moved at least one field; the pane names the device it got. */
  clamped: boolean;
}

function clampExtent(value: number): number {
  return Math.min(Math.max(value, VIEWPORT_LIMITS.minExtent), VIEWPORT_LIMITS.maxExtent);
}

function clampDpr(value: number): number {
  return Math.min(Math.max(value, VIEWPORT_LIMITS.minDpr), VIEWPORT_LIMITS.maxDpr);
}

/** Backing-store bytes for one viewport: extent x DPR, four bytes a pixel. */
export function backingBytes(viewport: { width: number; height: number; dpr: number }): number {
  return viewport.width * viewport.height * viewport.dpr * viewport.dpr * 4;
}

/**
 * Holds one asked-for viewport to the limits.
 *
 * Refused input (`null`/`undefined`, a non-finite or non-positive extent or DPR) returns `null`: the
 * caller keeps whatever the pane was already showing, exactly as a refused `viewport.set` leaves the
 * old picture. A finite but over-limit value is clamped, not refused, and reports `clamped`.
 *
 * The byte cap reduces the DPR, never the extent: a clamp must not move what a breakpoint sees.
 */
export function clampViewport(
  requested: { width: number; height: number; dpr: number },
): Viewport | null {
  if (!requested) {
    return null;
  }
  const { width, height, dpr: rawDpr } = requested;
  if (!Number.isFinite(width) || !Number.isFinite(height) || width <= 0 || height <= 0) {
    return null;
  }
  if (!Number.isFinite(rawDpr) || rawDpr <= 0) {
    return null;
  }
  const clampedWidth = clampExtent(width);
  const clampedHeight = clampExtent(height);
  let clampedDpr = clampDpr(rawDpr);
  let clamped = clampedWidth !== width || clampedHeight !== height || clampedDpr !== rawDpr;
  const bytes = backingBytes({ width: clampedWidth, height: clampedHeight, dpr: clampedDpr });
  if (bytes > VIEWPORT_LIMITS.maxBackingBytes) {
    clampedDpr = Math.min(
      clampedDpr * Math.sqrt(VIEWPORT_LIMITS.maxBackingBytes / bytes),
      VIEWPORT_LIMITS.maxDpr,
    );
    clamped = true;
  }
  return { width: clampedWidth, height: clampedHeight, dpr: clampedDpr, clamped };
}

/**
 * The rectangle the toolbar asked for, held to the limits.
 *
 * `panes` with no usable value (a pane rendered before the toolbar exists) fall back to the default
 * device rather than to `null`, which is what keeps the first frame painting. A size the toolbar did
 * supply is passed through `clampViewport`, so an over-limit request is a named clamp rather than a
 * blank pane.
 */
export function requestedViewport(
  environment: () => { width: number; height: number; dpr: number } | null | undefined,
): Viewport {
  const requested = environment();
  const clamped = requested ? clampViewport(requested) : null;
  if (clamped === null) {
    return { width: VIEWPORT_WIDTH, height: VIEWPORT_HEIGHT, dpr: 1, clamped: false };
  }
  return clamped;
}

/**
 * Sizes `canvas`'s backing store to the viewport and device pixel ratio, and returns the physical
 * extent. The CSS box is set too: the backing store is the device-pixel size, and the box is the
 * logical one.
 *
 * The caller passes a value from `requestedViewport`/`clampViewport`, so the extent is already legal;
 * the clamp is re-applied here anyway (it is idempotent) so a caller that reaches this function with
 * a raw reading still cannot size the store past the limits.
 */
export function sizeToDevicePixels(
  canvas: HTMLCanvasElement,
  requested: { width: number; height: number; dpr: number },
): { dpr: number; width: number; height: number; clamped: boolean } {
  const viewport = clampViewport(requested) ?? {
    width: VIEWPORT_WIDTH,
    height: VIEWPORT_HEIGHT,
    dpr: 1,
    clamped: true,
  };
  const width = Math.round(viewport.width * viewport.dpr);
  const height = Math.round(viewport.height * viewport.dpr);
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
  canvas.style.width = `${viewport.width}px`;
  canvas.style.height = `${viewport.height}px`;
  return { dpr: viewport.dpr, width, height, clamped: viewport.clamped };
}
