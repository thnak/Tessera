// SPDX-License-Identifier: MIT

// One editor group's tab strip (issue #431, split groups #1025): the open files of a group as a real
// `role="tablist"`, a close button beside each tab, and the two ways to move a tab between groups -- a
// drag (HTML5 drag and drop, a custom type so a file or text drop elsewhere is ignored) and Alt+ArrowRight /
// Alt+ArrowLeft for the keyboard. A move only changes which group lists the path; see `editor-groups.js`.

import { useRef, type DragEvent, type ReactNode } from "react";
import type { ProjectFile } from "../lib/project";

export type GroupId = "main" | "side";

/** The drag payload type: a tab's project path. */
export const TAB_DRAG_TYPE = "application/x-tessera-tab";

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

/** The open files as tabs, with the unsaved marker and a close button each. */
export function EditorTabs({
  group,
  label,
  panelId,
  open,
  files,
  active,
  dirty,
  onSelect,
  onClose,
  onMove,
  actions = null,
}: {
  group: GroupId;
  label: string;
  /** The id of the group's `role="tabpanel"`. */
  panelId: string;
  open: readonly string[];
  files: readonly ProjectFile[];
  active: string;
  dirty: readonly string[];
  onSelect: (path: string) => void;
  onClose: (path: string) => void;
  /** A tab was dropped (or Alt+Arrow'd) into `target` at `index` (the end when omitted). */
  onMove: (path: string, target: GroupId, index?: number) => void;
  /** Group-level buttons after the tabs (Split, Join). Outside the tablist: a button there is not a tab. */
  actions?: ReactNode;
}) {
  const strip = useRef<HTMLDivElement | null>(null);
  const other: GroupId = group === "main" ? "side" : "main";
  const dropIndex = (event: DragEvent, dragged: string): number | undefined => {
    const tabs = [...(strip.current?.querySelectorAll<HTMLElement>(".editor-tab") ?? [])].filter(
      (tab) => tab.dataset.path !== dragged,
    );
    const index = tabs.findIndex((tab) => {
      const box = tab.getBoundingClientRect();
      return event.clientX < box.left + box.width / 2;
    });
    return index < 0 ? undefined : index;
  };
  return (
    <div
      className="tabrow"
      data-group={group}
      onDragOver={(event) => {
        if (event.dataTransfer.types.includes(TAB_DRAG_TYPE)) {
          event.preventDefault();
          event.dataTransfer.dropEffect = "move";
        }
      }}
      onDrop={(event) => {
        const path = event.dataTransfer.getData(TAB_DRAG_TYPE);
        if (path !== "") {
          event.preventDefault();
          onMove(path, group, dropIndex(event, path));
        }
      }}
    >
    <div className="tabstrip" role="tablist" aria-label={label} ref={strip}>
      {open.map((path) => {
        const file = files.find((candidate) => candidate.path === path);
        const name = file ? file.name : path;
        const selected = path === active;
        const isDirty = dirty.includes(path);
        return (
          <div
            key={path}
            className={"editor-tab" + (selected ? " is-selected" : "")}
            data-path={path}
            draggable
            onDragStart={(event) => {
              event.dataTransfer.setData(TAB_DRAG_TYPE, path);
              event.dataTransfer.effectAllowed = "move";
            }}
            // A tab is a button (select) plus a button (close): the wrapper is the row, so the close
            // control is not nested inside the tab's own button -- interactive content inside a
            // `role="tab"` element is not a tab, it is a broken control.
          >
            <button
              id={tabIdOf(path)}
              type="button"
              role="tab"
              className="editor-tab-select"
              aria-controls={panelId}
              aria-selected={selected}
              tabIndex={selected ? 0 : -1}
              title={path}
              onClick={() => onSelect(path)}
              onKeyDown={(event) => {
                const index = open.indexOf(path);
                let next: string | null = null;
                if (event.altKey && (event.key === "ArrowRight" || event.key === "ArrowLeft")) {
                  // Alt+Arrow is the keyboard twin of the drag: right goes to the second group, left to the main one.
                  event.preventDefault();
                  if ((event.key === "ArrowRight") === (other === "side")) {
                    onMove(path, other);
                    requestAnimationFrame(() => document.getElementById(tabIdOf(path))?.focus());
                  }
                  return;
                }
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
    {actions}
    </div>
  );
}
