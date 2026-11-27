// SPDX-License-Identifier: MIT

// The three preview panels, as dock panel contents (issue #516, W5 #560). They replace the preview column
// (`PreviewColumn.tsx`) that held a view switch and the three panes in one right-hand column: the dock's own
// tab strips are the switch now, and each pane is a panel the viewer can open, close and drag.
//
// **The panes are still refs, not React children.** `ViewPanes.tsx` and `CanvasPane.tsx` render every subtree a
// session fills as a childless leaf; this file only composes them with what belongs to the Canvas panel: the
// component picker and the device's hardware strip. ADR-054 decision 3 is unchanged: React owns the chrome,
// the engine owns the documents and the canvas, and the canvas element is created once per open of the panel.

import type { ReactNode } from "react";
import type { PaneRefs } from "../page-state";
import { geometryModeOf } from "../views/skin-geometry";
import type { Skin } from "../views/skins";
import { CanvasPane } from "./CanvasPane";
import { DiagnosticsPane, ScenePane } from "./ViewPanes";

/** Every ref the preview column hands to the three sessions' panes. */
export type PreviewRefs = Pick<
  PaneRefs,
  | "diagStatus"
  | "diagList"
  | "diagEmpty"
  | "sceneStatus"
  | "nodes"
  | "sceneEmpty"
  | "script"
  | "inputStatus"
  | "cells"
  | "runInput"
  | "canvasStatus"
  | "canvasUnsupported"
  | "canvasUnsupportedBody"
  | "canvasFrame"
  | "canvas"
  | "canvasFailed"
  | "canvasFailedBody"
  | "canvasStats"
  | "statFrame"
  | "statDraws"
  | "statVertices"
  | "statNodes"
  | "statDpr"
  | "statFont"
  // Issue #444: the state row, which the canvas pane fills with the running evaluation's cells.
  | "stateRow"
  | "statState"
  // Issue #171's picker: the overlay the outline is drawn into, and the toggle that arms it.
  | "pickOverlay"
  | "pickToggle"
  // Issue #171's style inspector: the panel, the body it fills, and the toggle that arms it.
  | "inspectPanel"
  | "inspectBody"
  | "inspectToggle"
  // Issue #433: the fold/safe-area overlay and the toggle that arms it.
  | "foldOverlay"
  | "foldToggle"
>;


export function DiagnosticsPanel({ refs }: { refs: PreviewRefs }) {
  return <DiagnosticsPane statusRef={refs.diagStatus} listRef={refs.diagList} emptyRef={refs.diagEmpty} />;
}

export function ScenePanel({ refs }: { refs: PreviewRefs }) {
  return (
    <ScenePane
      statusRef={refs.sceneStatus}
      nodesRef={refs.nodes}
      emptyRef={refs.sceneEmpty}
      scriptRef={refs.script}
      inputStatusRef={refs.inputStatus}
      cellsRef={refs.cells}
      runInputRef={refs.runInput}
    />
  );
}

export function CanvasPanel({
  refs,
  skin,
  overlay,
  strip,
}: {
  refs: PreviewRefs;
  /** The selected skin, or `null` for no frame. */
  skin: Skin | null;
  /** Issue #448: the overlay over the canvas while the app is not in the foreground. */
  overlay: ReactNode;
  /** Issue #448: the device's hardware strip, generated from the profile's `controls`. */
  strip: ReactNode;
}) {
  // The geometry mode of section 5.1 is a property of the pair (skin, project): a desktop skin is `system` or
  // `custom` depending on `window.titleBar`, and a mobile skin is always `mobile`.
  const mode = skin === null ? "system" : geometryModeOf(skin, "system");
  return (
    <div className="canvas-panel">
      <CanvasPane
        statusRef={refs.canvasStatus}
        unsupportedRef={refs.canvasUnsupported}
        unsupportedBodyRef={refs.canvasUnsupportedBody}
        frameRef={refs.canvasFrame}
        canvasRef={refs.canvas}
        failedRef={refs.canvasFailed}
        failedBodyRef={refs.canvasFailedBody}
        frameStatsRef={refs.canvasStats}
        statFrameRef={refs.statFrame}
        statDrawsRef={refs.statDraws}
        statVerticesRef={refs.statVertices}
        statNodesRef={refs.statNodes}
        statDprRef={refs.statDpr}
        statFontRef={refs.statFont}
        stateRowRef={refs.stateRow}
        statStateRef={refs.statState}
        pickOverlayRef={refs.pickOverlay}
        pickToggleRef={refs.pickToggle}
        inspectPanelRef={refs.inspectPanel}
        inspectBodyRef={refs.inspectBody}
        inspectToggleRef={refs.inspectToggle}
        foldOverlayRef={refs.foldOverlay}
        foldToggleRef={refs.foldToggle}
        skin={skin}
        mode={mode}
        appOverlay={overlay}
      />
      {strip}
    </div>
  );
}
