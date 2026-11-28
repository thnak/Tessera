// SPDX-License-Identifier: MIT

// Region 3 of the studio workspace (issue #431): the **code column** -- the editor, with one tab per open
// project file.
//
// **One buffer per tab.** The page holds the text (`page-workspace.ts`), this component renders the tab
// strip and one CodeMirror 6 editor (`editor/code-editor.ts`, issue #517), and switching a tab opens that
// file in the editor in an effect; the editor keeps each open file's own undo history. What the page
// compiles is the active file's text, not one shared scratch buffer.
//
// **Two tab strips on the page, and neither is the page's navigation.** The editor's tabs are a real
// `role="tablist"` over the open buffers; the preview switch is the other one, in the column it controls.
// The page itself is not a tablist any more, which is the whole point of the issue: switching a preview
// view must not change the workspace's shape.
//
// The editor is uncontrolled on purpose. React would otherwise re-render the tree and the preview
// column on every keystroke; instead the page writes the buffer on each user edit, which marks the file
// dirty once. That is the same rule the canvas follows (ADR-054 decision 3): React owns the chrome, and a
// high-frequency write does not go through its render cycle.

import { useEffect, useRef, type ReactNode } from "react";
import { createCodeEditor, type CodeEditor } from "../editor/code-editor";
import { STALE_TEXT, type ProjectFile } from "../lib/project";

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
    case "mock":
      return "mock";
    case "services":
      return "svc";
    default:
      return "file";
  }
}

const VIEW_NOTES: Record<string, string> = {
  binary: "it is not UTF-8 text",
  "too-large": "it is over the 1 MiB file limit",
  unreadable: "the browser could not read it (it may have changed on disk)",
};

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
 * The code column: the tab strip, the editor and the notes below it.
 *
 * Since issue #444 an edit needs no button -- it applies ~150 ms after the last keystroke -- and since issue #557
 * the cold start is a command (`run.restart`, on the toolbar and in the Run menu), not a button here.
 */
export function CodeRegion({
  open,
  files,
  active,
  dirty,
  text,
  revision,
  sourceRef,
  onSelectTab,
  onCloseTab,
  onEdit,
  applying,
  onSave,
  onApplyNow,
  stale = [],
  empty = null,
  caretRef,
}: {
  open: readonly string[];
  files: readonly ProjectFile[];
  active: string;
  dirty: readonly string[];
  /** The active file's text: what the editor shows after a tab switch or a programmatic replace. */
  text: string;
  /**
   * Bumped by the page whenever it wrote a buffer itself (a tab switch, a sample).
   *
   * The editor is uncontrolled, so typing does not come back through here: a keystroke is written to
   * the page's buffer and never re-sent. Only a *page* write bumps this, which is what lets the effect
   * tell "the active file changed" from "the visitor typed", and it is why the editor costs one
   * re-render per file switch instead of one per key.
   */
  revision: number;
  /** Receives the editor once it is mounted; the sessions reveal spans and mark diagnostics through it. */
  sourceRef: React.MutableRefObject<CodeEditor | null>;
  onSelectTab: (path: string) => void;
  onCloseTab: (path: string) => void;
  onEdit: (text: string) => void;
  /** A live edit is scheduled or running: the hint says so instead of pretending nothing is happening. */
  applying: boolean;
  /** Mod-S in the editor: save to the opened folder (#391). */
  onSave: () => void;
  /** Mod-Enter in the editor: apply now, skipping the debounce. */
  onApplyNow: () => void;
  /** Generated files that may be out of date (advisory). */
  stale?: readonly string[];
  /** Shown instead of the editor while no project is open (the welcome view); the editor stays mounted. */
  empty?: ReactNode;
  /** The status bar's caret cell; the editor writes `Ln 3, Col 5 (12 selected)` into it without a render. */
  caretRef?: React.RefObject<HTMLSpanElement | null>;
}) {
  // The editor is created once, in an effect (it needs the DOM, and the Node render of the chrome has none).
  // Its callbacks go through a ref so the page's handlers can change without recreating the editor.
  const host = useRef<HTMLDivElement | null>(null);
  const handlers = useRef({ onEdit, onSave, onApplyNow });
  handlers.current = { onEdit, onSave, onApplyNow };
  const caret = useRef(caretRef);
  caret.current = caretRef;
  useEffect(() => {
    const parent = host.current;
    if (parent === null) {
      return;
    }
    const editor = createCodeEditor(parent, {
      onChange: (changed) => handlers.current.onEdit(changed),
      onSave: () => handlers.current.onSave(),
      onApplyNow: () => handlers.current.onApplyNow(),
      label: "Source code",
      onCaret: ({ line, column, selected }) => {
        const cell = caret.current?.current;
        if (cell) {
          cell.textContent = `Ln ${line}, Col ${column}` + (selected > 0 ? ` (${selected} selected)` : "");
        }
      },
    });
    sourceRef.current = editor;
    // A seam for the Playwright probes (they cannot set a value on a CodeMirror surface): the text, and an
    // edit that arrives exactly as typing does. Nothing on the page reads it.
    (globalThis as { __tesseraEditor?: CodeEditor }).__tesseraEditor = editor;
    return () => {
      delete (globalThis as { __tesseraEditor?: CodeEditor }).__tesseraEditor;
      sourceRef.current = null;
      editor.destroy();
    };
  }, [sourceRef]);

  // Opening a file from an effect keeps the text out of React's render cycle. It runs only on a page write:
  // the active file, or a buffer the page replaced. (The editor reuses a file's own state, history included,
  // when the text the page hands over is the text the editor already holds.)
  const applied = useRef<string>("");
  useEffect(() => {
    const editor = sourceRef.current;
    const stamp = `${revision}:${active}`;
    if (!editor || applied.current === stamp) {
      return;
    }
    applied.current = stamp;
    if (active !== "") {
      editor.open(active, text, files.find((file) => file.path === active)?.viewOnly === true);
    }
  }, [revision, active, text, files, sourceRef]);
  const current = files.find((file) => file.path === active) ?? null;

  return (
    <section className="region region-code" aria-label="Code">
      {empty === null ? (
        <EditorTabs
          open={open}
          files={files}
          active={active}
          dirty={dirty}
          onSelect={onSelectTab}
          onClose={onCloseTab}
        />
      ) : null}

      <div
        className="editor"
        id="editor-panel"
        hidden={empty !== null}
        role="tabpanel"
        aria-labelledby={active ? tabIdOf(active) : undefined}
      >
        <div id="editor-host" ref={host} />
      </div>
      {empty}

      <p className="hint" id="apply-status" role="status" aria-live="polite" hidden={!applying}>
        applying…
      </p>
      {current !== null && stale.includes(current.path) ? (
        <p className="hint stale-note" id="editor-stale" role="status">
          {STALE_TEXT}
        </p>
      ) : null}
      {current?.viewOnly ? (
        <p className="hint view-only-note" id="editor-view-only" data-path={current.path}>
          <span className="tree-lock">locked</span> <code>{current.path}</code> is read-only: the studio never edits,
          saves or previews it.{" "}
          {current.viewNote !== undefined
            ? `Not shown: ${VIEW_NOTES[current.viewNote] ?? current.viewNote}.`
            : current.viewText === undefined
              ? "Reading..."
              : ""}
        </p>
      ) : null}
    </section>
  );
}
