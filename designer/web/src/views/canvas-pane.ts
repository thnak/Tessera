// SPDX-License-Identifier: MIT

// The canvas pane's four states (P3, issue #167; the React host is issue #422).
//
// The states are one subject, which is why they are one module:
//
//   unsupported  the browser has no `navigator.gpu`, or no adapter/device could be acquired, or the
//                module could not take the handles the page handed it. The pane *names the reason* and
//                points at the Scene tab, which needs no GPU. This is the default content of the pane,
//                because a blank canvas is the failure this slice exists to avoid.
//   failed       the module loaded and a frame could not be drawn; the message is the module's own.
//   frame        a device exists and the last frame produced counters worth showing.
//   (nothing)    the pane is idle before the first Run on this view.
//
// **These are the only canvas elements React renders.** The status line, the two boxes and the frame
// counters are chrome -- sentences and numbers -- so React owns them and this module updates them through
// refs. The `<canvas>` inside the frame is different: React renders the element once and never again, and
// the session below binds the WebGPU context to it. That is ADR-054 decision 3 in one sentence: React
// renders the studio chrome, the engine draws the preview canvas.

import type { CanvasFrameReport } from "./canvas-session";
import type { StatusReporter } from "../lib/views";

/** The elements React committed for the Canvas pane. */
export interface CanvasPaneElements {
  status: HTMLElement;
  unsupported: HTMLElement;
  unsupportedBody: HTMLElement;
  frame: HTMLElement;
  canvas: HTMLCanvasElement;
  failed: HTMLElement;
  failedBody: HTMLElement;
  frameStats: HTMLElement;
  statFrame: HTMLElement;
  statDraws: HTMLElement;
  statVertices: HTMLElement;
  statNodes: HTMLElement;
  statDpr: HTMLElement;
}

export interface CanvasPane {
  elements: CanvasPaneElements;
  setStatus: StatusReporter;
  /**
   * Shows the named unsupported state. `reason` is a sentence from `canvas-device.ts` (or the view),
   * never a bare code: the point of the state is that a designer learns something from it.
   */
  showUnsupported(reason: string): void;
  showFailed(message: string): void;
  hideStates(): void;
  /** Shows what a frame produced: the counters, and a named state when it produced nothing. */
  renderReport(report: CanvasFrameReport): void;
}

export function createCanvasPane(
  elements: CanvasPaneElements,
  setStatus: StatusReporter,
): CanvasPane {
  const pane = elements;

  function showUnsupported(reason: string): void {
    pane.unsupportedBody.textContent = reason;
    pane.unsupported.hidden = false;
    pane.frame.hidden = true;
    pane.frameStats.hidden = true;
    setStatus("WebGPU unavailable", "warning");
  }

  function showFailed(message: string): void {
    pane.failedBody.textContent = message;
    pane.failed.hidden = false;
    setStatus("failed", "warning");
  }

  function hideStates(): void {
    pane.unsupported.hidden = true;
    pane.failed.hidden = true;
  }

  /**
   * A rejected frame (`hasDevice: false`) is the module refusing the handles the page handed it, which is
   * the unsupported state rather than a generic failure; a frame with `drawCalls: 0` is a real answer that
   * simply is not a picture, and saying so is the same choice the Scene view makes about an empty
   * document.
   */
  function renderReport(report: CanvasFrameReport): void {
    if (report.hasDevice !== true) {
      showUnsupported(report.error || "The preview module got no WebGPU device.");
      return;
    }
    hideStates();
    pane.frame.hidden = false;
    pane.frameStats.hidden = false;
    pane.statFrame.textContent = report.width + "x" + report.height;
    pane.statDraws.textContent = String(report.drawCalls);
    pane.statVertices.textContent = String(report.vertices);
    pane.statNodes.textContent = String(report.nodes);
    if (report.error) {
      showFailed(report.error);
    } else if (report.drawCalls === 0) {
      setStatus("no draw calls (the source produced no painted boxes)", "warning");
    } else {
      setStatus("drew " + report.drawCalls + " draw call(s)", "ok");
    }
  }

  return { elements: pane, setStatus, showUnsupported, showFailed, hideStates, renderReport };
}
