// SPDX-License-Identifier: MIT

// Region 4 of the studio workspace (issue #431): the **preview column**, the right-hand third of the
// workspace.
//
// **This is where the page's one remaining view switch lives.** Before #431 the three views were three
// top-level tabs, so opening Canvas replaced the editor and the page's shape changed under the visitor.
// Now the switch is a tablist *inside the preview column*: it selects which preview the column shows and
// nothing outside the column moves. That is the issue's whole acceptance sentence ("No view switch
// changes the top-level layout"), and it is why the tablist is here rather than in the header.
//
// The switch keeps every id the page contract names -- `tab-diagnostics`, `tab-scene`, `tab-canvas` and
// `view-diagnostics`, `view-scene`, `view-canvas` -- so `?view=`, `#p5=` share links and
// `scripts/ci/preview_page_checks.mjs` keep resolving. The tablist is one tab stop with a roving
// tabindex and the arrow keys move inside it, exactly as the top-level strip did.
//
// **The panes are still refs, not React children.** `ViewPanes.tsx` and `CanvasPane.tsx` render every
// subtree a session fills as a childless leaf; this component only decides which of the three is visible
// and where the environment toolbar sits. ADR-054 decision 3 is unchanged: React owns the chrome, the
// engine owns the documents and the canvas.

import { VIEWS, type ViewDefinition } from "../lib/views";
import type { PaneRefs } from "../page-state";
import { CanvasPane } from "./CanvasPane";
import { DiagnosticsPane, ScenePane } from "./ViewPanes";
import { EnvironmentToolbar, type ControlValues } from "./StudioChrome";

/** The preview's own view switch: scene / canvas / diagnostics, inside the column they fill. */
export function PreviewTabs({
  current,
  onSelect,
}: {
  current: ViewDefinition["id"];
  onSelect: (id: ViewDefinition["id"]) => void;
}) {
  return (
    <div className="preview-tabs" role="tablist" aria-label="Preview views">
      {VIEWS.map((view) => {
        const selected = view.id === current;
        return (
          <button
            key={view.id}
            id={`tab-${view.id}`}
            type="button"
            role="tab"
            className="tab"
            aria-controls={`view-${view.id}`}
            aria-selected={selected}
            // Roving tabindex: a tab strip is one tab stop, and the arrow keys move inside it.
            tabIndex={selected ? 0 : -1}
            onClick={() => onSelect(view.id)}
            onKeyDown={(event) => {
              const order = VIEWS.map((entry) => entry.id);
              const index = order.indexOf(current);
              let next: ViewDefinition["id"] | null = null;
              if (event.key === "ArrowRight") {
                next = order[(index + 1) % order.length];
              } else if (event.key === "ArrowLeft") {
                next = order[(index - 1 + order.length) % order.length];
              } else if (event.key === "Home") {
                next = order[0];
              } else if (event.key === "End") {
                next = order[order.length - 1];
              }
              if (next) {
                event.preventDefault();
                onSelect(next);
                // Moving focus with the selection is what makes the strip one tab stop that is usable:
                // without it the focus ring would stay on a tab that is no longer selected.
                document.getElementById(`tab-${next}`)?.focus();
              }
            }}
          >
            {view.label}
          </button>
        );
      })}
    </div>
  );
}

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
>;

export function PreviewColumn({
  view,
  onSelectView,
  controls,
  onControlChange,
  refs,
  components,
  component,
  onPickComponent,
}: {
  view: ViewDefinition;
  onSelectView: (id: ViewDefinition["id"]) => void;
  controls: ControlValues;
  onControlChange: (name: keyof ControlValues, value: string) => void;
  refs: PreviewRefs;
  /** The entry's declared components (the compiler's list) and the one previewed. */
  components: readonly string[];
  component: string;
  onPickComponent: (name: string) => void;
}) {
  return (
    <section className="region region-preview" aria-labelledby="preview-heading">
      <div className="region-header">
        <h2 id="preview-heading">Preview</h2>
        <PreviewTabs current={view.id} onSelect={onSelectView} />
      </div>

      {/* The environment the preview is laid out and painted in. It belongs to this column: two of the
          three panes read it as a layout input, and the third carries it into the share link. */}
      <EnvironmentToolbar controls={controls} onChange={onControlChange} />

      {components.length > 1 ? (
        <label className="component-picker">
          Component
          <select
            id="component-select"
            value={component}
            onChange={(event) => onPickComponent(event.currentTarget.value)}
          >
            {components.map((name) => (
              <option key={name} value={name}>
                {name}
              </option>
            ))}
          </select>
        </label>
      ) : null}

      <div className="preview-well">
        <DiagnosticsPane
          view={view}
          statusRef={refs.diagStatus}
          listRef={refs.diagList}
          emptyRef={refs.diagEmpty}
        />
        <ScenePane
          view={view}
          statusRef={refs.sceneStatus}
          nodesRef={refs.nodes}
          emptyRef={refs.sceneEmpty}
          scriptRef={refs.script}
          inputStatusRef={refs.inputStatus}
          cellsRef={refs.cells}
          runInputRef={refs.runInput}
        />
        <CanvasPane
          view={view}
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
        />
      </div>
    </section>
  );
}
