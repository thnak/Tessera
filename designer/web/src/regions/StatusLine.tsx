// SPDX-License-Identifier: MIT

// Region 5 of the studio workspace (issue #431): the **status line**, spanning the bottom of the
// workspace.
//
// The spec's status line is richer than this page's (`designer-studio-v1.md` section 3: engine id,
// backend, simulation state, diagnostics count, frame time) because the simulation controls it describes
// (#408, #171) are not part of the studio yet. What the issue's layout asks for is the line itself --
// build id, diagnostics count, frame counters -- plus, now that the workspace has files, which file Run
// compiles.
//
// **Two of the cells are written by the sessions, not by React.** A compile and a GPU frame must not
// re-render the page (ADR-054 decision 3), so the diagnostics count and the frame reading arrive through
// refs the page passes down as `diagnosticsRef`/`frameRef`. The build id and the compile target are React
// state and are rendered normally.
//
// The line is a real region, not a paragraph under the panes: the grid keeps it at the bottom of the
// viewport whatever a column does, and on a phone it is the last block of the single column.

import type { RefObject } from "react";

/** The line's cells: one `<dt>`/`<dd>` pair each, in reading order. */
function Cell({
  label,
  id,
  valueRef,
  children,
}: {
  label: string;
  id: string;
  valueRef?: RefObject<HTMLSpanElement | null>;
  children: React.ReactNode;
}) {
  const valueId = id;
  return (
    <div className="status-cell">
      <dt>{label}</dt>
      <dd>
        <span id={valueId} ref={valueRef}>
          {children}
        </span>
      </dd>
    </div>
  );
}

export function StatusLine({
  buildId,
  buildIdTitle,
  buildIdRef,
  diagnosticsRef,
  frameRef,
  file,
  onToggleDiagnostics,
  diagnosticsOpen,
  hidden,
}: {
  /** `view.toggleStatusBar` and zen mode hide the line; its cells keep their refs, so sessions still write them. */
  hidden?: boolean;
  /** Makes the Diagnostics cell a button that toggles the panel area (`view.togglePanelArea`). */
  onToggleDiagnostics?: () => void;
  diagnosticsOpen?: boolean;
  buildId: string;
  buildIdTitle?: string;
  buildIdRef: RefObject<HTMLSpanElement | null>;
  /** The diagnostics count: written by the diagnostics session when it compiles. */
  diagnosticsRef: RefObject<HTMLSpanElement | null>;
  /** The last frame's reading: written by the canvas session when it draws. */
  frameRef: RefObject<HTMLSpanElement | null>;
  /** The canonical project path Run compiles (issue #431), or `""` before the project settles. */
  file: string;
}) {
  return (
    <footer className="page-footer" aria-label="Status" hidden={hidden}>
      <dl className="status-line">
        <div className="status-cell">
          <dt>Build</dt>
          <dd>
            <span id="build-id" ref={buildIdRef} title={buildIdTitle}>
              {buildId}
            </span>
          </dd>
        </div>
        <Cell label="Compile target" id="status-file">
          <code>{file === "" ? "none" : file}</code>
        </Cell>
        {/* The panel area is collapsed by default, so this cell is also the toggle that opens it (W5, #560;
            VS Code's Problems item): a collapsed dock never hides an error. */}
        {onToggleDiagnostics === undefined ? (
          <Cell label="Diagnostics" id="status-diagnostics" valueRef={diagnosticsRef}>
            not run yet
          </Cell>
        ) : (
          <div className="status-cell">
            <dt>Diagnostics</dt>
            <dd>
              <button
                type="button"
                className="status-toggle"
                id="status-diagnostics-toggle"
                aria-pressed={diagnosticsOpen === true}
                aria-label="Toggle the panel area (Diagnostics)"
                title="Show or hide the panel area (Ctrl+J)"
                onClick={onToggleDiagnostics}
              >
                <span id="status-diagnostics" ref={diagnosticsRef}>
                  not run yet
                </span>
              </button>
            </dd>
          </div>
        )}
        <Cell label="Frame" id="status-frame" valueRef={frameRef}>
          not drawn
        </Cell>
      </dl>
      <p className="status-note">
        Part of <a href="https://github.com/thnak/Tessera">Tessera</a>. The preview panes are three Wasm
        modules built from this repository: <code>tessera_diagnostics</code>,{" "}
        <code>tessera_scene_preview</code> and <code>tessera_canvas</code>. Each is checked against its
        native counterpart by a CI gate, byte for byte where the output is a document.
      </p>
    </footer>
  );
}
