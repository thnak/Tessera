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

import type { CssDiagnostic } from "./css-diagnostics";

/** What one frame reports; the pane renders these counters. */
export interface CanvasFrameReport {
  hasDevice?: boolean;
  width: number;
  height: number;
  /**
   * The session's lifetime frame count (`CanvasSceneSession::frames_painted()`).
   *
   * This is the number the live edit's contract is written in: "one frame per burst", "zero frames
   * while static". The pane shows it so a browser probe (and a developer) can check the rule against
   * the engine's own counter instead of inferring it from a repaint.
   */
  frames?: number;
  /**
   * How many times the running evaluation's cascade has been resolved (issue #436). The pane shows
   * it beside the frame count, because "a resize that crosses no `@media` breakpoint does not
   * rebuild the cascade" is a claim about this number: it moves by exactly one per crossed
   * breakpoint and not at all for a resize that stays inside one.
   */
  cascade?: number;
  drawCalls: number;
  vertices: number;
  nodes: number;
  error?: string;
  /**
   * The author stylesheet's problems (issue #390), appended to the report rather than replacing any
   * field of it: `canvasVersion` stays 1, and a page built against the previous module simply never
   * reads this. Absent from an older module, which the pane treats as "no problems to show".
   */
  cssDiagnostics?: CssDiagnostic[];
  /** True when the module hit its cap and `cssDiagnostics` is not the whole list. */
  cssDiagnosticsTruncated?: boolean;
}

/** One state cell of the running evaluation, as `tessera_canvas_apply` reports it. */
export interface CanvasStateCell {
  name: string;
  /** A scalar cell's value; absent for a list cell, which carries `items` instead. */
  value?: boolean | number | string;
  items?: Array<boolean | number | string>;
}

/**
 * What one *apply* reports (issue #444), from `tessera_canvas_apply`.
 *
 * `changed` is the module's own answer to "is a frame owed", so an automatic apply of unchanged text
 * costs zero frames while an edit costs exactly one. `error` non-empty means the source did not
 * evaluate and the running evaluation was **kept**: the page shows the diagnostic and draws nothing,
 * which is why the canvas cannot blank while a developer is typing. The carry counters are
 * `StateCarryReport`'s split (`tools/codegen/scene_state.hpp`): `droppedComponent` non-zero is the
 * spec's structural-change reset.
 *
 * `state` is the running evaluation's cells after the apply. The canvas pane prints them, because "a
 * state cell survives a text edit" is something a developer must be able to *see* rather than
 * believe: before #444 the canvas had no state display at all.
 */
export interface CanvasApplyReport {
  changed: boolean;
  reused: boolean;
  carried: number;
  droppedComponent: number;
  droppedCell: number;
  droppedType: number;
  added: number;
  /** The session's lifetime frame count, so the page can report the cost of an edit. */
  frames?: number;
  state?: CanvasStateCell[];
  error: string;
}

/**
 * What one *viewport change* reports (issue #436), from `tessera_canvas_set_viewport`.
 *
 * `changed` is true when a layout pass ran, so the page draws one frame; a viewport already in
 * force answers false and costs nothing. `cascadeGeneration` is the session's own count of cascade
 * resolutions: it starts at 1 per Run and moves by exactly one when a size crosses an `@media`
 * breakpoint, so it is the engine's evidence that a resize that crosses none was a layout pass
 * rather than a re-resolve. `width`/`height`/`dpr` are what the session was asked for, so the pane
 * can report the device it actually got.
 */
export interface CanvasViewportReport {
  changed: boolean;
  width: number;
  height: number;
  dpr: number;
  cascadeGeneration?: number;
  error: string;
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
