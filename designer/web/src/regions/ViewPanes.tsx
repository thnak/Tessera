// SPDX-License-Identifier: MIT

// Region 3 of `designer-studio-v1.md` section 3: the **workspace pane**, as far as #392 and #422 have
// built it -- the shared source editor and the three view panes it feeds.
//
// #391 (file tree, editor, save) replaces the plain textarea with a real editor and adds the tree; nothing
// here blocks that, because the editor is one component with one ref and the panes take the textarea as a
// layout input rather than owning it. What matters now is the invariant this file keeps: **one editor for
// all three views**, so the same source is compiled, laid out and painted without being copied between
// pages. That was the merge's product claim (#392) and it survives the conversion unchanged.
//
// **Every subtree the engine writes into is rendered childless and by ref.** The two `<ol>`s, the empty
// states, the input list and the canvas are leaves React renders once; the sessions fill them
// (`src/views/*-session.ts`). That is what lets React own the chrome without putting compiled documents or
// a GPU surface inside its render cycle (ADR-054 decision 3).

import type { RefObject } from "react";
import type { ViewDefinition } from "../lib/views";

/** The page's one source editor, plus the actions and the per-view hint beneath it. */
export function EditorPane({
  sourceRef,
  editorHintRef,
  onRun,
  onLoadSample,
  runDisabled,
  onKeyDown,
}: {
  sourceRef: RefObject<HTMLTextAreaElement | null>;
  editorHintRef: RefObject<HTMLParagraphElement | null>;
  onRun: () => void;
  onLoadSample: () => void;
  runDisabled: boolean;
  onKeyDown: (event: React.KeyboardEvent<HTMLTextAreaElement>) => void;
}) {  return (
    <section className="pane editor-pane" aria-labelledby="editor-heading">
      <div className="pane-header">
        <h2 id="editor-heading">Source</h2>
        <div className="pane-actions">
          <button id="run" type="button" className="primary" onClick={onRun} disabled={runDisabled}>
            Run
          </button>
          <button id="load-sample" type="button" onClick={onLoadSample}>
            Load sample
          </button>
        </div>
      </div>
      <label htmlFor="source" className="visually-hidden">
        TSX source
      </label>
      <textarea
        id="source"
        ref={sourceRef}
        spellCheck={false}
        autoCapitalize="off"
        autoComplete="off"
        autoCorrect="off"
        wrap="off"
        onKeyDown={onKeyDown}
      />
      {/* The hint belongs to the *page*, not to a view: the editor is shared, and a hint that said "a
          diagnostics run has no viewport" while the Scene tab was open would be wrong. The page swaps it
          per active view; the Scene view appends the live rectangle to it. */}
      <p className="hint" id="editor-hint" ref={editorHintRef}>
        Ctrl+Enter (or Cmd+Enter) runs.
      </p>
    </section>
  );
}

/** View 1: the diagnostics list (P1, issue #165). The default pane and the first-paint one. */
export function DiagnosticsPane({
  view,
  statusRef,
  listRef,
  emptyRef,
}: {
  view: ViewDefinition;
  statusRef: RefObject<HTMLDivElement | null>;
  listRef: RefObject<HTMLOListElement | null>;
  emptyRef: RefObject<HTMLDivElement | null>;
}) {
  return (
    <section
      className="pane view"
      id="view-diagnostics"
      role="tabpanel"
      aria-labelledby="tab-diagnostics"
      tabIndex={0}
      hidden={view.id !== "diagnostics"}
    >
      <div className="pane-header">
        <h2>Diagnostics</h2>
        <div className="status" id="diag-status" role="status" aria-live="polite" ref={statusRef}>
          not run yet
        </div>
      </div>
      <ol className="diagnostics" id="diagnostics" ref={listRef} />
      <div className="empty-state" id="diag-empty" ref={emptyRef}>
        Press <strong>Run</strong> to compile the source.
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
  view,
  statusRef,
  nodesRef,
  emptyRef,
  scriptRef,
  inputStatusRef,
  cellsRef,
  runInputRef,
}: {
  view: ViewDefinition;
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
      role="tabpanel"
      aria-labelledby="tab-scene"
      tabIndex={0}
      hidden={view.id !== "scene"}
    >
      <div className="pane-header">
        <h2>Scene snapshot</h2>
        <div className="status" id="scene-status" role="status" aria-live="polite" ref={statusRef}>
          not run yet
        </div>
      </div>
      <ol className="nodes" id="nodes" ref={nodesRef} />
      <div className="empty-state" id="scene-empty" ref={emptyRef}>
        Press <strong>Run</strong> to evaluate the source.
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
