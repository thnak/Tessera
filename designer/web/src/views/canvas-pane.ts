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
  /** The font row's value cell (issue #387): the face's name, or the reason text is absent. */
  statFont: HTMLElement;
  /**
   * The status line's frame cell (issue #431): the same reading as the frame counters, in the one place
   * that stays visible after a view switch. Optional, because a pane without a status line is a valid
   * pane (the SSR chrome renders one, and the smoke test does not need the write).
   */
  statusFrame: HTMLElement | null;
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
  /**
   * Shows which face the frame is painted with (issue #387), or that text is not painted at all.
   *
   * `family` null means "no font": either the fetch failed (`reason` names why) or the module refused the
   * bytes. The row is never blank, because "text is missing from the canvas" is exactly the fact a
   * designer needs attributed -- an empty row would read as a rendering bug rather than a missing font.
   */
  setFontState(family: string | null, reason?: string): void;
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
    // The status line is the one cell that survives a view switch, so it is never left describing a
    // frame that is no longer there.
    if (pane.statusFrame) {
      pane.statusFrame.textContent = "WebGPU unavailable";
    }
  }

  function showFailed(message: string): void {
    pane.failedBody.textContent = message;
    pane.failed.hidden = false;
    setStatus("failed", "warning");
    if (pane.statusFrame) {
      pane.statusFrame.textContent = "failed";
    }
  }

  function hideStates(): void {
    pane.unsupported.hidden = true;
    pane.failed.hidden = true;
  }

  function setFontState(family: string | null, reason?: string): void {
    if (family !== null) {
      pane.statFont.textContent = family;
      pane.statFont.title = "Text is painted with these font bytes, fetched from this site.";
      return;
    }
    // A short cell plus the whole sentence in `title`, so the row stays a table and the reason is
    // still reachable without widening the pane.
    pane.statFont.textContent = "none (text not drawn)";
    pane.statFont.title = reason ?? "No font was loaded, so text runs are counted but not painted.";
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
    if (pane.statusFrame) {
      pane.statusFrame.textContent = `${report.width}x${report.height}, ${report.drawCalls} draw call(s)`;
    }
    if (report.error) {
      showFailed(report.error);
    } else if (report.drawCalls === 0) {
      setStatus("no draw calls (the source produced no painted boxes)", "warning");
    } else {
      setStatus("drew " + report.drawCalls + " draw call(s)", "ok");
    }
  }

  return { elements: pane, setStatus, showUnsupported, showFailed, hideStates, setFontState, renderReport };
}
