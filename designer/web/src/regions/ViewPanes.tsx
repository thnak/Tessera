// SPDX-License-Identifier: MIT

// The three preview panes that fill the right-hand column of the studio workspace (issue #431 moved them
// out of the top-level tablist; #392 and #422 built them).
//
// **Every subtree the engine writes into is rendered childless and by ref.** The two `<ol>`s, the empty
// states, the input list and the canvas are leaves React renders once; the sessions fill them
// (`src/views/*-session.ts`). That is what lets React own the chrome without putting compiled documents or
// a GPU surface inside its render cycle (ADR-054 decision 3).
//
// The panes are `hidden` by the *view*, not unmounted: the Canvas view holds a WebGPU surface and the
// Scene view holds a Wasm instance, and neither may be destroyed because a visitor looked at Diagnostics.
// `regions/PreviewColumn.tsx` is what switches between them.

import type { RefObject } from "react";

/** View 1: the diagnostics list (P1, issue #165). The default pane and the first-paint one. */
export function DiagnosticsPane({
  statusRef,
  listRef,
  emptyRef,
}: {
  statusRef: RefObject<HTMLDivElement | null>;
  listRef: RefObject<HTMLOListElement | null>;
  emptyRef: RefObject<HTMLDivElement | null>;
}) {
  return (
    <section
      className="pane view"
      id="view-diagnostics"
    >
      <div className="pane-header">
        <h2>Diagnostics</h2>
        <div className="status" id="diag-status" role="status" aria-live="polite" ref={statusRef}>
          not run yet
        </div>
      </div>
      <ol className="diagnostics" id="diagnostics" ref={listRef} />
      <div className="empty-state" id="diag-empty" ref={emptyRef}>
        The diagnostics for this file appear here as you type.
      </div>
    </section>
  );
}

/**
 * View 2: the scene snapshot and its input script (P2 issue #166, P4 issue #168).
 *
 * The pane keeps its own status lines as refs, and the node list, the cells and the empty state as
 * childless leaves -- the scene session writes all of them.
 */
export function ScenePane({
  statusRef,
  nodesRef,
  emptyRef,
  scriptRef,
  inputStatusRef,
  cellsRef,
  runInputRef,
}: {
  statusRef: RefObject<HTMLDivElement | null>;
  nodesRef: RefObject<HTMLOListElement | null>;
  emptyRef: RefObject<HTMLDivElement | null>;
  scriptRef: RefObject<HTMLTextAreaElement | null>;
  inputStatusRef: RefObject<HTMLDivElement | null>;
  cellsRef: RefObject<HTMLOListElement | null>;
  runInputRef: RefObject<HTMLButtonElement | null>;
}) {
  return (
    <section
      className="pane view"
      id="view-scene"
    >
      <div className="pane-header">
        <h2>Scene snapshot</h2>
        <div className="status" id="scene-status" role="status" aria-live="polite" ref={statusRef}>
          not run yet
        </div>
      </div>
      <ol className="nodes" id="nodes" ref={nodesRef} />
      <div className="empty-state" id="scene-empty" ref={emptyRef}>
        The evaluated scene appears here as you type.
      </div>
      <p className="hint" id="scene-hint">
        One line of the input script is one step: <code>click &lt;automationId&gt;</code>. Clicking a
        node appends its id. Each click is dispatched through the engine&rsquo;s own input router &mdash;
        hit test, pointer capture, click synthesis, capture/target/bubble &mdash; and the handler it
        reaches is the handler&rsquo;s <strong>TSX preview body</strong>, run by the IR evaluator. The
        developer&rsquo;s C++ is not in this module and is never executed.
      </p>

      <div className="pane-header">
        <h2 id="input-heading">Input script</h2>
        <div className="pane-actions">
          <button id="run-input" type="button" ref={runInputRef} disabled>
            Send clicks
          </button>
        </div>
      </div>
      <label htmlFor="script" className="visually-hidden">
        Input script
      </label>
      <textarea
        id="script"
        ref={scriptRef}
        spellCheck={false}
        autoCapitalize="off"
        autoComplete="off"
        autoCorrect="off"
        wrap="off"
        rows={4}
      />

      <div className="pane-header">
        <h2 id="state-heading">State cells</h2>
        <div className="status" id="input-status" role="status" aria-live="polite" ref={inputStatusRef}>
          no clicks yet
        </div>
      </div>
      <ol className="cells" id="cells" ref={cellsRef} />
    </section>
  );
}
