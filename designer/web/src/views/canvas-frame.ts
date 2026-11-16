// SPDX-License-Identifier: MIT

// The Canvas view's frame report and the sizing rule (P3, issue #167; the React host is issue #422).
//
// Split out of `canvas-session.ts` so the session stays inside the source-size limit, and because these two
// things are independent of any session: the report is the module's C ABI shape, and the sizing rule is the
// device-pixel arithmetic both the canvas and its status line need.
//
// The logical viewport is the Wasm export's fixed 320x240 (and the native CLI's default); a moved toolbar
// overrides it. The *physical* size is that rectangle times the device pixel ratio, which is what the
// canvas' backing store is set to -- a canvas whose backing store is its CSS size is blurry on every
// high-density display, and a preview judged on blurry pixels is not a preview.

/** What one frame reports; the pane renders these counters. */
export interface CanvasFrameReport {
  hasDevice?: boolean;
  width: number;
  height: number;
  drawCalls: number;
  vertices: number;
  nodes: number;
  error?: string;
}

/** The logical viewport the preview lays out at when the toolbar has not moved. */
export const VIEWPORT_WIDTH = 320;
export const VIEWPORT_HEIGHT = 240;

/** The rectangle the toolbar asked for, with sane fallbacks for a malformed value. */
export function requestedViewport(
  environment: () => { width: number; height: number; dpr: number },
): { width: number; height: number; dpr: number } {
  const requested = environment();
  if (!requested || !requested.width || !requested.height) {
    return { width: VIEWPORT_WIDTH, height: VIEWPORT_HEIGHT, dpr: 1 };
  }
  return requested;
}

/**
 * Sizes `canvas`'s backing store to the requested viewport and device pixel ratio, and returns the physical
 * extent. The CSS box is set too: the backing store is the device-pixel size, and the box is the logical one.
 */
export function sizeToDevicePixels(
  canvas: HTMLCanvasElement,
  requested: { width: number; height: number; dpr: number },
): { dpr: number; width: number; height: number } {
  const dpr = requested.dpr && requested.dpr > 0 ? requested.dpr : 1;
  const width = Math.round(requested.width * dpr);
  const height = Math.round(requested.height * dpr);
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
  canvas.style.width = `${requested.width}px`;
  canvas.style.height = `${requested.height}px`;
  return { dpr, width, height };
}
