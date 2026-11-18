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

import type { CanvasFrameReport, CanvasStateCell } from "./canvas-session";
import { describeCss, describeCssDetails, type CssDiagnostic } from "./css-diagnostics";
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
   * The state row's value cell (issue #444): the running evaluation's cells, as the apply reported
   * them. The row is hidden while there is no state to show.
   */
  stateRow: HTMLElement;
  statState: HTMLElement;
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
  /**
   * Shows the running evaluation's state cells (issue #444).
   *
   * This is what makes a carried cell *observable* on the pane the edit is about: a developer who
   * changes a string in a component with a `count` cell can see the count stay where it was, and can
   * see it go back to its initializer after **Restart app**. Before this the canvas showed no state
   * at all, so "state survives the edit" was only checkable in the Scene pane's click script.
   *
   * An empty list clears the row rather than printing "none": a source with no state cells has
   * nothing to report, and a row that said "none" would look like a failed carry.
   */
  renderState(cells: readonly CanvasStateCell[]): void;
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
   *
   * **A stylesheet problem is not a failed frame** (issue #390). The parser is tolerant, so a bad rule
   * costs that rule: the report's `cssDiagnostics` are shown *beside* the counters, and the frame is
   * still drawn. Only `error` -- "no frame at all" -- reaches `showFailed`.
   */
  function renderReport(report: CanvasFrameReport): void {
    if (report.hasDevice !== true) {
      showUnsupported(report.error || "The preview module got no WebGPU device.");
      return;
    }
    hideStates();
    pane.frame.hidden = false;
    pane.frameStats.hidden = false;
    // The frame count comes from the session, not from this pane: it is the engine's own tally of
    // what it painted, which is the number the issue's "one frame per burst, zero when static"
    // acceptance is about. Shown beside the size so a developer can watch it stay put while a static
    // preview is left alone. `cN` is the cascade generation (#436): a viewport change that crosses
    // an `@media` breakpoint moves it by one, and one that does not leaves it alone.
    const cascade = report.cascade === undefined ? "" : `, c${report.cascade}`;
    pane.statFrame.textContent =
      report.frames === undefined ? report.width + "x" + report.height
                                  : `${report.width}x${report.height} (#${report.frames}${cascade})`;
    pane.statDraws.textContent = String(report.drawCalls);
    pane.statVertices.textContent = String(report.vertices);
    pane.statNodes.textContent = String(report.nodes);
    if (pane.statusFrame) {
      pane.statusFrame.textContent =
        `${report.width}x${report.height}, ${report.drawCalls} draw call(s)` +
        (report.frames === undefined ? "" : `, ${report.frames} frame(s)`);
    }
    const problems = report.cssDiagnostics ?? [];
    renderCssProblems(pane, problems, report.cssDiagnosticsTruncated === true);
    if (report.error) {
      showFailed(report.error);
      return;
    }
    if (report.drawCalls === 0) {
      setStatus(
        problems.length === 0
          ? "no draw calls (the source produced no painted boxes)"
          : cssStatus(problems, "no draw calls"),
        "warning",
      );
      return;
    }
    setStatus(
      problems.length === 0
        ? "drew " + report.drawCalls + " draw call(s)"
        : cssStatus(problems, "drew " + report.drawCalls + " draw call(s)"),
      problems.length === 0 ? "ok" : "warning",
    );
  }

  /**
   * The state row (#444): one `name=value` pair per cell, in declaration order.
   *
   * A cell's name is the IR id (`state::count`), which is what the carry rule matches on, so a
   * developer reading this row is reading the identity the engine used. `data-cell` carries the raw
   * name and `data-value` the scalar, so a probe can assert a cell's value without parsing prose.
   */
  function renderState(cells: readonly CanvasStateCell[]): void {
    if (cells.length === 0) {
      pane.statState.replaceChildren();
      pane.statState.removeAttribute("data-value");
      pane.statState.hidden = true;
      pane.stateRow.hidden = true;
      return;
    }
    const nodes = cells.map((cell) => {
      const row = document.createElement("span");
      row.className = "state-cell";
      row.dataset.cell = cell.name;
      const shown = cell.items !== undefined ? cell.items.join(",") : String(cell.value ?? "");
      row.dataset.value = shown;
      row.textContent = `${cell.name}=${shown}`;
      return row;
    });
    pane.statState.replaceChildren(...nodes);
    // The single-scalar case gets its value on the row itself, which is what a one-cell sample
    // (the Counter) needs to be assertable without hunting for the span.
    const scalar = cells.length === 1 && cells[0].items === undefined ? cells[0].value : undefined;
    if (scalar === undefined) {
      pane.statState.removeAttribute("data-value");
    } else {
      pane.statState.dataset.value = String(scalar);
    }
    pane.statState.hidden = false;
    pane.stateRow.hidden = false;
  }

  return {
    elements: pane,
    setStatus,
    showUnsupported,
    showFailed,
    hideStates,
    setFontState,
    renderReport,
    renderState,
  };
}

/**
 * The stylesheet problems, in the pane's failed area (issue #390).
 *
 * They go in `failedBody`, which is the pane's own diagnostics area and already styled as a named
 * state, and the *heading* stays hidden: the stylesheet is a warning, not a failed preview. That
 * combination is deliberate -- the paragraph is visible with the problems while the frame above it is
 * still the author's screen, which is exactly "a bad rule must not blank the frame".
 *
 * Writing into a React-rendered element's `textContent` is the same rule every session follows: React
 * rendered it childless and never reconciles it again (ADR-054 decision 3).
 */
function renderCssProblems(
  pane: CanvasPaneElements,
  problems: CssDiagnostic[],
  truncated: boolean,
): void {
  if (problems.length === 0) {
    pane.failedBody.textContent = "";
    return;
  }
  const details = describeCssDetails({
    cssVersion: 2,
    styled: true,
    truncated,
    diagnostics: problems,
  });
  pane.failedBody.textContent = details + (truncated ? "\n(more problems than the report carries)" : "");
  // The box is shown so the text is visible, but the heading is not the failure heading: React owns
  // its text, so it is left alone and the box reads as the diagnostics list it is.
  pane.failed.hidden = false;
}

/** `drew 4 draw call(s) · css 2 problem(s)` -- one sentence, the counters first. */
function cssStatus(problems: CssDiagnostic[], unless: string): string {
  return unless + describeCss({
    cssVersion: 2,
    styled: true,
    truncated: false,
    diagnostics: problems,
  });
}
