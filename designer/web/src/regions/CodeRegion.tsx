// SPDX-License-Identifier: MIT

// Region 3 of the studio workspace (issue #431): the **code column** -- the editor, with one tab per open
// project file.
//
// **One buffer per tab.** The page holds the text (`page-workspace.ts`), this component renders the tab
// strip and one `<textarea>`, and switching a tab swaps the textarea's content in an effect. A textarea
// rather than a real editor is deliberate (a code editor needs a dependency, and #431 keeps the
// no-new-dependency rule): what this issue changes is that the editor is *per file* and that the active
// file's text is what Run compiles, not one shared scratch buffer.
//
// **Two tab strips on the page, and neither is the page's navigation.** The editor's tabs are a real
// `role="tablist"` over the open buffers; the preview switch is the other one, in the column it controls.
// The page itself is not a tablist any more, which is the whole point of the issue: switching a preview
// view must not change the workspace's shape.
//
// The textarea is uncontrolled on purpose. React would otherwise re-render the tree and the preview
// column on every keystroke; instead the page writes the buffer on `input`, which marks the file dirty
// once. That is the same rule the canvas follows (ADR-054 decision 3): React owns the chrome, and a
// high-frequency write does not go through its render cycle.

import { useEffect, useRef } from "react";
import type { ProjectFile } from "../lib/project";

/** The id of the tab for `path` (`aria-controls`/`aria-labelledby` need one, and a path has slashes). */
export function tabIdOf(path: string): string {
  return "tab-file-" + path.replace(/[^A-Za-z0-9]+/g, "-").replace(/^-|-$/g, "");
}

/** The letter a tab shows for a file's kind, so a `.tsx` is distinguishable from a `.css` by shape. */
function kindLetter(kind: string): string {
  switch (kind) {
    case "source":
      return "tsx";
    case "stylesheet":
      return "css";
    case "config":
      return "json";
    case "theme":
      return "theme";
    default:
      return "file";
  }
}

/** The open files as tabs, with the unsaved marker and a close button each. */
function EditorTabs({
  open,
  files,
  active,
  dirty,
  onSelect,
  onClose,
}: {
  open: readonly string[];
  files: readonly ProjectFile[];
  active: string;
  dirty: readonly string[];
  onSelect: (path: string) => void;
  onClose: (path: string) => void;
}) {
  return (
    <div className="tabstrip" role="tablist" aria-label="Open files">
      {open.map((path) => {
        const file = files.find((candidate) => candidate.path === path);
        const name = file ? file.name : path;
        const selected = path === active;
        const isDirty = dirty.includes(path);
        return (
          <div
            key={path}
            className={"editor-tab" + (selected ? " is-selected" : "")}
            // A tab is a button (select) plus a button (close): the wrapper is the row, so the close
            // control is not nested inside the tab's own button -- interactive content inside a
            // `role="tab"` element is not a tab, it is a broken control.
          >
            <button
              id={tabIdOf(path)}
              type="button"
              role="tab"
              className="editor-tab-select"
              aria-controls="editor-panel"
              aria-selected={selected}
              tabIndex={selected ? 0 : -1}
              title={path}
              onClick={() => onSelect(path)}
              onKeyDown={(event) => {
                const index = open.indexOf(path);
                let next: string | null = null;
                if (event.key === "ArrowRight") {
                  next = open[(index + 1) % open.length];
                } else if (event.key === "ArrowLeft") {
                  next = open[(index - 1 + open.length) % open.length];
                } else if (event.key === "Home") {
                  next = open[0];
                } else if (event.key === "End") {
                  next = open[open.length - 1];
                } else if (event.key === "Delete" || (event.key === "w" && (event.ctrlKey || event.metaKey))) {
                  event.preventDefault();
                  onClose(path);
                  return;
                }
                if (next !== null) {
                  event.preventDefault();
                  onSelect(next);
                  document.getElementById(tabIdOf(next))?.focus();
                }
              }}
            >
              <span className="editor-tab-kind" aria-hidden="true">
                {kindLetter(file ? file.kind : "other")}
              </span>
              <span className="editor-tab-name">{name}</span>
              {isDirty ? (
                <span className="editor-tab-dirty" title="Unsaved changes" aria-label="Unsaved changes">
                  &bull;
                </span>
              ) : null}
            </button>
            <button
              type="button"
              className="editor-tab-close"
              aria-label={`Close ${name}`}
              title={`Close ${name}`}
              onClick={() => onClose(path)}
            >
              &times;
            </button>
          </div>
        );
      })}
    </div>
  );
}

/**
 * The code column: the tab strip, the editor, the actions above the text and the hint below it.
 *
 * The actions (Run, Load sample) are here rather than in the preview column because they act on *this*
 * text: Run compiles the file the editor shows, and the preview column only displays the result. The
 * `run`/`load-sample` ids are the page's contract (`scripts/ci/preview_page_checks.mjs`) and stay.
 */
export function CodeRegion({
  open,
  files,
  active,
  dirty,
  text,
  revision,
  sourceRef,
  stylesheetRef,
  editorHintRef,
  onSelectTab,
  onCloseTab,
  onEdit,
  onRun,
  onLoadSample,
  runDisabled,
  onKeyDown,
}: {
  open: readonly string[];
  files: readonly ProjectFile[];
  active: string;
  dirty: readonly string[];
  /** The active file's text: what the textarea holds after a tab switch or a programmatic replace. */
  text: string;
  /**
   * Bumped by the page whenever it wrote a buffer itself (a tab switch, a sample, a share link).
   *
   * The textarea is uncontrolled, so typing does not come back through here: a keystroke is written to
   * the page's buffer and never re-sent. Only a *page* write bumps this, which is what lets the effect
   * tell "the active file changed" from "the visitor typed", and it is why the editor costs one
   * re-render per file switch instead of one per key.
   */
  revision: number;
  sourceRef: React.RefObject<HTMLTextAreaElement | null>;
  /** The author stylesheet's textarea (issue #390), rendered below `#source` in this same column. */
  stylesheetRef: React.RefObject<HTMLTextAreaElement | null>;
  editorHintRef: React.RefObject<HTMLParagraphElement | null>;
  onSelectTab: (path: string) => void;
  onCloseTab: (path: string) => void;
  onEdit: (text: string) => void;
  onRun: () => void;
  onLoadSample: () => void;
  runDisabled: boolean;
  onKeyDown: (event: React.KeyboardEvent<HTMLTextAreaElement>) => void;
}) {
  // Writing the textarea from an effect (not a `value` prop) keeps it out of React's render cycle. The
  // effect runs only on a page write: the active file, or a buffer the page replaced.
  const applied = useRef<string>("");
  useEffect(() => {
    const node = sourceRef.current;
    const stamp = `${revision}:${active}`;
    if (!node || applied.current === stamp) {
      return;
    }
    applied.current = stamp;
    node.value = text;
    node.scrollTop = 0;
  }, [revision, active, text, sourceRef]);
  const current = files.find((file) => file.path === active) ?? null;

  return (
    <section className="region region-code" aria-labelledby="editor-heading">
      <div className="region-header">
        <h2 id="editor-heading">Code</h2>
        <div className="pane-actions">
          <button id="run" type="button" className="primary" onClick={onRun} disabled={runDisabled}>
            Run
          </button>
          <button id="load-sample" type="button" onClick={onLoadSample}>
            Load sample
          </button>
        </div>
      </div>

      <EditorTabs
        open={open}
        files={files}
        active={active}
        dirty={dirty}
        onSelect={onSelectTab}
        onClose={onCloseTab}
      />

      <div
        className="editor"
        id="editor-panel"
        role="tabpanel"
        aria-labelledby={active ? tabIdOf(active) : undefined}
      >
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
          onInput={(event) => onEdit(event.currentTarget.value)}
        />
      </div>

      {/* The author stylesheet (issue #390). A second textarea rather than a mode switch or a tab,
          because the two are edited together: the source is what the screen *is* and the stylesheet is
          what it *looks like*, and Run sends both. It is filed as `PREVIEW_STYLESHEET_FILE`
          (`ui/Preview.css`), so a source's `import "./Preview.css";` is this textarea, and the sheet
          applies even without that import. It is not a project file -- opening `styles/theme.css` in a
          tab edits that file, which the preview does not compile yet (#391) -- so the sheet lives here,
          beside the source it styles, rather than in the file tree.

          The value is uncontrolled and read at Run time, like `#source`: the page's `SourceReader`
          drains it into the snapshot each session receives (ADR-054 decision 3). A stylesheet in React
          state would re-render the page on every keystroke, around a live WebGPU surface. */}
      <div className="editor-subheader">
        <h2 id="stylesheet-heading">Stylesheet</h2>
      </div>
      <label htmlFor="stylesheet" className="visually-hidden">
        Author stylesheet, applied as ui/Preview.css
      </label>
      <textarea
        id="stylesheet"
        ref={stylesheetRef}
        spellCheck={false}
        autoCapitalize="off"
        autoComplete="off"
        autoCorrect="off"
        wrap="off"
        rows={6}
        aria-describedby="stylesheet-note"
      />
      <p className="hint" id="stylesheet-note">
        Applied as <code>ui/Preview.css</code>. Any CSS here <strong>replaces</strong> the built-in
        preview sheet; leave it empty to keep the built-in look.
      </p>

      {/* The hint belongs to the page, not to a view: the editor is shared by the three preview panes,
          and a hint that said "a diagnostics run has no viewport" while the preview showed the canvas
          would be wrong. The page swaps it per active preview view; the Scene session appends the live
          rectangle to it. */}
      <p className="hint" id="editor-hint" ref={editorHintRef}>
        Ctrl+Enter (or Cmd+Enter) runs.
      </p>
      {current ? (
        <p className="hint" id="editor-file" data-path={current.path}>
          Editing <code>{current.path}</code>
          {dirty.includes(current.path) ? " (unsaved changes)" : ""}
        </p>
      ) : null}
    </section>
  );
}
