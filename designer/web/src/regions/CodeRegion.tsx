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
// **Split groups (#1025).** The area can hold two groups, each its own tab strip and its own CodeMirror
// editor. A file lives in exactly one group, so the page's per-path buffer stays the single copy: moving a tab
// moves a path, the dirty set and the canvas never see it. The focused group's editor is `sourceRef` (what the
// sessions reveal spans and mark diagnostics through); focus follows the pointer or the keyboard into a group.
//
// The editor is uncontrolled on purpose. React would otherwise re-render the tree and the preview
// column on every keystroke; instead the page writes the buffer on each user edit, which marks the file
// dirty once. That is the same rule the canvas follows (ADR-054 decision 3): React owns the chrome, and a
// high-frequency write does not go through its render cycle.

import { useEffect, useRef, type ReactNode } from "react";
import { createCodeEditor, type CodeEditor } from "../editor/code-editor";
import { breadcrumbs } from "../lib/editor-tabs";
import { STALE_TEXT, type ProjectFile } from "../lib/project";
import { activeOf, canSplit, groupTabs } from "../../editor-groups.js";
import { EditorTabs, tabIdOf, type GroupId } from "./EditorTabs";

export { tabIdOf };

const VIEW_NOTES: Record<string, string> = {
  binary: "it is not UTF-8 text",
  "too-large": "it is over the 1 MiB file limit",
  unreadable: "the browser could not read it (it may have changed on disk)",
};

interface GroupHandlers {
  onEdit: (text: string, path: string) => void;
  onSave: () => void;
  onApplyNow: () => void;
}

/** One group: its tab strip, its tabpanel and its own CodeMirror editor (created in an effect, see the header). */
function EditorGroup({
  group,
  tabs,
  files,
  active,
  focused,
  dirty,
  textOf,
  revision,
  handlers,
  onReady,
  preview,
  onSelect,
  onPin,
  onClose,
  onMove,
  onFocus,
  caretRef,
  disabled,
  actions,
}: {
  group: GroupId;
  tabs: readonly string[];
  files: readonly ProjectFile[];
  active: string;
  focused: boolean;
  dirty: readonly string[];
  textOf: (path: string) => string;
  revision: number;
  handlers: React.MutableRefObject<GroupHandlers>;
  onReady: (group: GroupId, editor: CodeEditor | null) => void;
  preview: string | null;
  onSelect: (path: string) => void;
  onPin: (path: string) => void;
  onClose: (path: string) => void;
  onMove: (path: string, target: GroupId, index?: number) => void;
  onFocus: (path: string) => void;
  caretRef?: React.RefObject<HTMLSpanElement | null>;
  disabled: boolean;
  actions: ReactNode;
}) {
  const host = useRef<HTMLDivElement | null>(null);
  const editorRef = useRef<CodeEditor | null>(null);
  const activeRef = useRef(active);
  activeRef.current = active;
  const caret = useRef(caretRef);
  caret.current = caretRef;
  const side = group === "side";
  useEffect(() => {
    const parent = host.current;
    if (parent === null) {
      return;
    }
    const editor = createCodeEditor(parent, {
      onChange: (changed) => handlers.current.onEdit(changed, activeRef.current),
      onSave: () => handlers.current.onSave(),
      onApplyNow: () => handlers.current.onApplyNow(),
      label: side ? "Source code, second group" : "Source code",
      id: side ? "source-side" : "source",
      onCaret: ({ line, column, selected }) => {
        const cell = caret.current?.current;
        if (cell && focusedRef.current) {
          cell.textContent = `Ln ${line}, Col ${column}` + (selected > 0 ? ` (${selected} selected)` : "");
        }
      },
    });
    editorRef.current = editor;
    onReady(group, editor);
    return () => {
      onReady(group, null);
      editorRef.current = null;
      editor.destroy();
    };
  }, [group, side, handlers, onReady]);
  const focusedRef = useRef(focused);
  focusedRef.current = focused;

  // Opening a file from an effect keeps the text out of React's render cycle. It runs only on a page write:
  // the group's active file, or a buffer the page replaced. (The editor reuses a file's own state, history
  // included, when the text the page hands over is the text the editor already holds.)
  const applied = useRef<string>("");
  useEffect(() => {
    const editor = editorRef.current;
    const stamp = `${revision}:${active}`;
    if (!editor || applied.current === stamp) {
      return;
    }
    applied.current = stamp;
    if (active !== "") {
      editor.open(active, textOf(active), files.find((file) => file.path === active)?.viewOnly === true);
    }
  }, [revision, active, textOf, files]);

  const suffix = side ? "-side" : "";
  return (
    <div
      className={"editor-group" + (focused ? " is-focused" : "")}
      data-group={group}
      onFocusCapture={() => active !== "" && !focused && onFocus(active)}
    >
      {disabled ? null : (
        <EditorTabs
          group={group}
          label={side ? "Open files, second group" : "Open files"}
          panelId={"editor-panel" + suffix}
          open={tabs}
          files={files}
          active={active}
          dirty={dirty}
          preview={preview}
          onSelect={onSelect}
          onPin={onPin}
          onClose={onClose}
          onMove={onMove}
          actions={actions}
        />
      )}
      {disabled || active === "" ? null : (
        <nav className="breadcrumbs" id={"editor-breadcrumbs" + suffix} aria-label="Breadcrumbs">
          <ol>
            {breadcrumbs(active).map((crumb) => (
              <li key={crumb.path} aria-current={crumb.file ? "page" : undefined} title={crumb.path}>
                {crumb.name}
              </li>
            ))}
          </ol>
        </nav>
      )}
      <div
        className="editor"
        id={"editor-panel" + suffix}
        hidden={disabled}
        role="tabpanel"
        aria-labelledby={active ? tabIdOf(active) : undefined}
      >
        <div id={"editor-host" + suffix} ref={host} />
      </div>
    </div>
  );
}

/**
 * The code column: the tab strips, the editors and the notes below them.
 *
 * Since issue #444 an edit needs no button -- it applies ~150 ms after the last keystroke -- and since issue #557
 * the cold start is a command (`run.restart`, on the toolbar and in the Run menu), not a button here.
 */
export function CodeRegion({
  groups,
  files,
  dirty,
  textOf,
  preview = null,
  onPinTab = () => {},
  revision,
  sourceRef,
  onSelectTab,
  onCloseTab,
  onFocusTab,
  onMoveTab,
  onSplit,
  onJoin,
  onEdit,
  applying,
  onSave,
  onApplyNow,
  stale = [],
  empty = null,
  caretRef,
}: {
  /** The workspace's tab state (`open`, `active` and the group fields, see `editor-groups.js`). */
  groups: Parameters<typeof groupTabs>[0];
  files: readonly ProjectFile[];
  dirty: readonly string[];
  /** The text of a file: what an editor shows after a tab switch, a move or a programmatic replace. */
  textOf: (path: string) => string;
  /** The open tab that is a replaceable preview (italic); a double click pins it (#562). */
  preview?: string | null;
  onPinTab?: (path: string) => void;
  /**
   * Bumped by the page whenever it wrote a buffer itself (a tab switch, a sample).
   *
   * The editor is uncontrolled, so typing does not come back through here: a keystroke is written to
   * the page's buffer and never re-sent. Only a *page* write bumps this, which is what lets the effect
   * tell "the active file changed" from "the visitor typed", and it is why the editor costs one
   * re-render per file switch instead of one per key.
   */
  revision: number;
  /** Receives the focused group's editor; the sessions reveal spans and mark diagnostics through it. */
  sourceRef: React.MutableRefObject<CodeEditor | null>;
  onSelectTab: (path: string) => void;
  onCloseTab: (path: string) => void;
  /** Focus moved into the group showing `path` (no content change, so no revision bump). */
  onFocusTab: (path: string) => void;
  onMoveTab: (path: string, group: GroupId, index?: number) => void;
  onSplit: () => void;
  onJoin: () => void;
  /** A user edit of `path`'s buffer (the group's own file, not necessarily the focused one). */
  onEdit: (text: string, path: string) => void;
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
  /** The status bar's caret cell; the focused editor writes `Ln 3, Col 5 (12 selected)` into it without a render. */
  caretRef?: React.RefObject<HTMLSpanElement | null>;
}) {
  const handlers = useRef<GroupHandlers>({ onEdit, onSave, onApplyNow });
  handlers.current = { onEdit, onSave, onApplyNow };
  const tabs = groupTabs(groups);
  const split = tabs.side.length > 0;
  const mainActive: string = activeOf(groups, "main");
  const sideActive: string = activeOf(groups, "side");
  const focusSide = split && tabs.side.includes(groups.active);

  // The focused group's editor is what the sessions and the probes see; republished when focus or the split changes.
  const editors = useRef<Record<GroupId, CodeEditor | null>>({ main: null, side: null });
  const focusRef = useRef<GroupId>("main");
  focusRef.current = focusSide ? "side" : "main";
  const publish = useRef(() => {
    const editor = editors.current[focusRef.current] ?? editors.current.main;
    sourceRef.current = editor;
    // A seam for the Playwright probes (they cannot set a value on a CodeMirror surface): the text, and an
    // edit that arrives exactly as typing does. Nothing on the page reads it.
    const seam = globalThis as { __tesseraEditor?: CodeEditor };
    if (editor) {
      seam.__tesseraEditor = editor;
    } else {
      delete seam.__tesseraEditor;
    }
  });
  const onReady = useRef((group: GroupId, editor: CodeEditor | null) => {
    editors.current[group] = editor;
    publish.current();
  });
  useEffect(() => publish.current(), [focusSide, split]);
  useEffect(() => () => {
    sourceRef.current = null;
  }, [sourceRef]);

  const current = files.find((file) => file.path === groups.active) ?? null;
  const moveLabel = (group: GroupId) => (group === "main" ? "Split editor right" : "Merge editor groups");
  const actionOf = (group: GroupId) =>
    group === "main" && !split ? (
      <button type="button" className="editor-group-action" id="editor-split" title={moveLabel(group)}
        aria-label={moveLabel(group)} disabled={!canSplit(groups)} onClick={onSplit}>
        &#x2AFF;
      </button>
    ) : group === "side" ? (
      <button type="button" className="editor-group-action" id="editor-join" title={moveLabel(group)}
        aria-label={moveLabel(group)} onClick={onJoin}>
        &#x2715;
      </button>
    ) : null;
  const shared = { files, dirty, textOf, revision, handlers, preview, onPin: onPinTab, onReady: onReady.current, onSelect: onSelectTab, onClose: onCloseTab, onMove: onMoveTab, onFocus: onFocusTab, caretRef };

  return (
    <section className="region region-code" aria-label="Code">
      <div className={"editor-groups" + (split ? " is-split" : "")}>
        <EditorGroup {...shared} group="main" tabs={tabs.main} active={mainActive} focused={!focusSide}
          disabled={empty !== null} actions={actionOf("main")} />
        {split ? (
          <EditorGroup {...shared} group="side" tabs={tabs.side} active={sideActive} focused={focusSide}
            disabled={false} actions={actionOf("side")} />
        ) : null}
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
