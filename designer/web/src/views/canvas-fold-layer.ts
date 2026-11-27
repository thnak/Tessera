// SPDX-License-Identifier: MIT

// The fold-hinge and safe-area overlay's DOM half (issue #433): a toggle, the boxes, and the caption.
//
// **What it is.** A debug aid drawn over the preview that shows the hinge band of a simulated fold
// and the safe-area, keyboard and cutout insets the tree was laid out inside. Off by default, so a
// visitor who never opens it sees exactly what they saw before.
//
// **Page-only, by construction.** It is DOM the browser composites over the canvas, so it is not in
// the engine's frame: it cannot reach a golden, it is not in any byte `tessera_canvas_draw` paints,
// and it appears in a page screenshot only because the browser composites the page - which is why
// `designer-studio-v1.md` section 6 assigns it "excluded from screenshots" as a rule for the MCP
// artifact path, not for the browser's own compositor. The engine's contribution is a *query*
// (`_tessera_canvas_overlay`): it dispatches nothing, marks nothing dirty and owes no frame.
//
// **Why the geometry comes from the engine.** `canvas-fold.ts` explains it: the band is a layout
// input the engine resolved, so a page that recomputed it could disagree with the boxes it is
// drawing over.
//
// **Why `pointer-events: none` is load-bearing** (the same rule the picker's overlay follows). The
// overlay covers the canvas exactly; if it accepted pointer events it would swallow every move and
// press the moment it was armed, and the preview would stop responding.

import { callExport, type WasmModule } from "../lib/wasm-call";
import type { StatusReporter } from "../lib/views";
import { overlayBoxes, overlayCaption, type CanvasOverlayReport } from "./canvas-fold";

export interface CanvasFoldOptions {
  /** The overlay element the boxes are drawn into. Rendered childless; this module owns its content. */
  overlay: HTMLElement;
  /** The pane's toggle. Null when the pane has no header, which the SSR chrome's pane does not. */
  toggle: HTMLButtonElement | null;
  /** The canvas, for the CSS-to-logical ratio. */
  canvas: HTMLCanvasElement;
  /** The logical viewport the running session was laid out at. */
  viewport: () => { width: number; height: number };
  /** The instantiated module, or null before it loads. */
  module: () => WasmModule | null;
  setStatus: StatusReporter;
}

export interface CanvasFold {
  /** Whether the overlay is armed. Off by default: a debug aid, not the preview. */
  armed(): boolean;
  setArmed(armed: boolean): void;
  toggle(): void;
  /**
   * Re-asks the engine and redraws.
   *
   * Called on a posture, viewport or skin change, and when the toggle arms: every one of those moves
   * the geometry the overlay is drawn from. A disarmed overlay does nothing, so a toolbar move costs
   * no query when the tool is closed.
   */
  refresh(): void;
  /** Hides the boxes without disarming: the pane went away or the session was released. */
  hide(): void;
}

export function createCanvasFold(options: CanvasFoldOptions): CanvasFold {
  const { overlay, toggle, canvas } = options;
  let isArmed = false;
  // Built once and reused: a box per draw would be a DOM allocation on a path a resize runs, and the
  // boxes are replaced wholesale anyway (`replaceChildren`).
  let caption: HTMLElement | null = null;

  function hide(): void {
    overlay.hidden = true;
  }

  /**
   * The overlay's CSS pixels per logical pixel of the report.
   *
   * `offsetWidth` rather than `getBoundingClientRect().width`, for the same reason the picker does:
   * the pane zooms the stage with a CSS transform, which scales the rect but not the layout box, and
   * the overlay's own `px` units are layout ones. At 100% the two agree.
   */
  function cssPerLogical(logicalWidth: number): number {
    const cssWidth = canvas.offsetWidth;
    if (cssWidth <= 0 || logicalWidth <= 0) {
      return 1;
    }
    return cssWidth / logicalWidth;
  }

  function draw(report: CanvasOverlayReport): void {
    const scale = cssPerLogical(report.width);
    const boxes = overlayBoxes(report, scale);
    const children: HTMLElement[] = [];
    for (const box of boxes) {
      const element = document.createElement("div");
      element.className = `fold-overlay-box fold-overlay-${box.kind}`;
      element.style.left = `${box.left}px`;
      element.style.top = `${box.top}px`;
      element.style.width = `${box.width}px`;
      element.style.height = `${box.height}px`;
      children.push(element);
    }
    if (caption === null) {
      caption = document.createElement("div");
      caption.className = "fold-overlay-caption";
      // The caption is a visual readout of a debug aid, never content: a screen reader announcing
      // "vertical hinge 24px" while browsing the page would be noise.
      caption.setAttribute("aria-hidden", "true");
    }
    caption.textContent = overlayCaption(report);
    // The caption sits at the bottom-left of the picture, where the hinge (centred) and the corner
    // insets (the edges) are not, so it is readable at every posture.
    children.push(caption);
    overlay.replaceChildren(...children);
    overlay.hidden = false;
  }

  function refresh(): void {
    if (!isArmed) {
      return;
    }
    const module = options.module();
    if (module === null) {
      // Armed before the module loaded: nothing to ask. The overlay stays hidden rather than showing
      // a stale device's geometry.
      hide();
      return;
    }
    let report: CanvasOverlayReport;
    try {
      report = JSON.parse(callExport(module, "_tessera_canvas_overlay")) as CanvasOverlayReport;
    } catch (error) {
      // A module built before this export existed, or a malformed document: the overlay goes away and
      // the pane says why, rather than shading a band nobody can account for.
      hide();
      options.setStatus("overlay unavailable: " + String(error), "warning");
      return;
    }
    // The report is the engine's `WindowMetrics`, so it describes the tree that is on screen. A
    // report whose extent does not match the viewport the page asked for means the session has not
    // been laid out at this size yet; drawing it would put every box in the wrong place.
    const viewport = options.viewport();
    if (report.width !== Math.round(viewport.width) || report.height !== Math.round(viewport.height)) {
      hide();
      return;
    }
    draw(report);
  }

  function setArmed(armed: boolean): void {
    isArmed = armed;
    if (toggle !== null) {
      toggle.setAttribute("aria-pressed", armed ? "true" : "false");
      toggle.textContent = armed ? "Overlay on" : "Overlay";
    }
    if (!armed) {
      hide();
      return;
    }
    options.setStatus("overlay armed: the hinge and the safe areas are drawn over the preview", null);
    refresh();
  }

  toggle?.addEventListener("click", () => {
    setArmed(!isArmed);
  });

  return {
    armed: () => isArmed,
    setArmed,
    toggle: () => setArmed(!isArmed),
    refresh,
    hide,
  };
}
