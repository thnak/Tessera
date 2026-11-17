// SPDX-License-Identifier: MIT

// View 3: the WebGPU canvas pane (P3, issue #167; the React host is issue #422).
//
// **The canvas is the one thing on this page the engine draws, and it is mounted exactly once.** React
// renders `<canvas>` here as a childless leaf with a ref, and this component's props never change in any
// way that would make React replace it: no `key`, no `children`, no conditional mount. The `width`/
// `height` attributes are a *constant* 320x240 initial hint and never vary between renders, so React
// sets them once and leaves the element alone; the session then owns the backing store and sizes it
// from its own `environment()` plus DPR on every run, because that is a device-pixel decision the
// module and the display share rather than React state.
//
// That is ADR-054 decision 3 in code: the four regions are components, and the engine's one surface is a
// ref'd host element that the canvas session owns from its first frame onwards. A preview crash loses the
// canvas, not the editor, and React's render cycle never runs during a GPU frame.
//
// The *states* around the canvas are chrome and therefore React's: the status line, the two boxes and the
// frame counters. The session updates them through refs rather than state, on purpose -- a status line that
// re-rendered the tree would put the canvas back into the render cycle it is deliberately outside of.

import type { RefObject } from "react";
import type { ViewDefinition } from "../lib/views";

export function CanvasPane({
  view,
  statusRef,
  unsupportedRef,
  unsupportedBodyRef,
  frameRef,
  canvasRef,
  failedRef,
  failedBodyRef,
  frameStatsRef,
  statFrameRef,
  statDrawsRef,
  statVerticesRef,
  statNodesRef,
  statDprRef,
  statFontRef,
  stateRowRef,
  statStateRef,
}: {
  view: ViewDefinition;
  statusRef: RefObject<HTMLDivElement | null>;
  unsupportedRef: RefObject<HTMLDivElement | null>;
  unsupportedBodyRef: RefObject<HTMLParagraphElement | null>;
  frameRef: RefObject<HTMLDivElement | null>;
  canvasRef: RefObject<HTMLCanvasElement | null>;
  failedRef: RefObject<HTMLDivElement | null>;
  failedBodyRef: RefObject<HTMLParagraphElement | null>;
  frameStatsRef: RefObject<HTMLDListElement | null>;
  statFrameRef: RefObject<HTMLElement | null>;
  statDrawsRef: RefObject<HTMLElement | null>;
  statVerticesRef: RefObject<HTMLElement | null>;
  statNodesRef: RefObject<HTMLElement | null>;
  statDprRef: RefObject<HTMLElement | null>;
  statFontRef: RefObject<HTMLElement | null>;
  /** Issue #444: the state row's `<dt>` and `<dd>`, hidden until a cell exists. */
  stateRowRef: RefObject<HTMLElement | null>;
  statStateRef: RefObject<HTMLElement | null>;
}) {
  return (
    <section
      className="pane view"
      id="view-canvas"
      role="tabpanel"
      aria-labelledby="tab-canvas"
      tabIndex={0}
      hidden={view.id !== "canvas"}
    >
      <div className="pane-header">
        <h2>Canvas</h2>
        <div className="status" id="canvas-status" role="status" aria-live="polite" ref={statusRef}>
          not started
        </div>
      </div>

      {/* The unsupported-browser state. It is hidden until the view is opened, and the view swaps in the
          canvas only once a device exists, so a browser without WebGPU sees this text and never an empty
          black rectangle. */}
      <div className="unavailable" id="unsupported" ref={unsupportedRef} hidden>
        <h3 id="unsupported-heading">This browser cannot show the canvas preview</h3>
        <p id="unsupported-body" ref={unsupportedBodyRef}>
          The preview draws through WebGPU, and this browser did not provide it.
        </p>
        <p>
          The <strong>Scene</strong> tab shows the same evaluated scene and needs no GPU at all.
        </p>
      </div>

      {/* The engine's one surface. React renders this element once and never touches it again; the
          canvas session owns its backing store, its WebGPU context and its frames. */}
      <div className="canvas-frame" id="canvas-frame" ref={frameRef} hidden>
        <canvas id="canvas" ref={canvasRef} width={320} height={240} aria-label="Preview of the evaluated scene" />
      </div>

      <div className="unavailable" id="failed" ref={failedRef} hidden>
        <h3>Preview failed</h3>
        <p id="failed-body" ref={failedBodyRef} />
      </div>

      <dl className="frame-stats" id="frame-stats" ref={frameStatsRef} hidden>
        <dt>Frame</dt>
        <dd id="stat-frame" ref={statFrameRef}>
          -
        </dd>
        <dt>Draws</dt>
        <dd id="stat-draws" ref={statDrawsRef}>
          -
        </dd>
        <dt>Vertices</dt>
        <dd id="stat-vertices" ref={statVerticesRef}>
          -
        </dd>
        <dt>Painted nodes</dt>
        <dd id="stat-nodes" ref={statNodesRef}>
          -
        </dd>
        <dt>Device pixel ratio</dt>
        <dd id="stat-dpr" ref={statDprRef}>
          -
        </dd>
        {/* Issue #387: which face the text is painted with. Part of the frame counters rather than a
            separate box, because "is text drawn, and with what" is a property of the frame the counters
            already describe. */}
        <dt>Font</dt>
        <dd id="stat-font" ref={statFontRef}>
          -
        </dd>
        {/* Issue #444: the running evaluation's state cells. In the counters rather than a separate
            box, for the same reason the font row is: "did the count survive the edit" is a property
            of the evaluation these counters already describe, and the row appears only when the
            source actually declares a cell. */}
        <dt id="state-row" ref={stateRowRef} hidden>
          State
        </dt>
        <dd id="stat-state" ref={statStateRef} hidden>
          -
        </dd>
      </dl>
    </section>
  );
}
