// SPDX-License-Identifier: MIT

// Region 2 of the studio workspace (issue #431): the **project tree**, the left column.
//
// It draws the files of the opened project and nothing else: the rows come from `lib/project.ts`, which
// built them from paths `project-paths.js` confined, and this component decides only how a row looks and
// what a keyboard press does to the selection. No file is read here; a row that is locked (a font, an
// image, C++) is shown with a badge and cannot be opened, which is the spec's accepted-file rule
// (`designer-studio-v1.md` section 7) made visible.
//
// **The tree is one tab stop and arrow-key navigable** (ADR-054 decision 8, #391's acceptance): the
// container is `role="tree"`, every row is a `role="treeitem"` with `aria-level`, `aria-selected` and --
// for a directory -- `aria-expanded`, and exactly one row carries `tabIndex=0` (the selected one). The
// arrow keys move the selection and the focus together, so the focus ring never sits on a row that is
// no longer selected.
//
// Nothing here is a session's: the tree is chrome, React renders it from state, and the page's own state
// (`page-workspace.ts`) is the only thing that changes. That keeps the engine/React split of ADR-054
// decision 3 intact -- the tree is not a document a Wasm module produced.

import { useRef, type ReactNode } from "react";
import type { TreeRow } from "../lib/project";

/** The index of the selected row, or 0 when it is not among the visible rows. */
function selectedIndex(rows: readonly TreeRow[], selected: string): number {
  const index = rows.findIndex((row) => row.path === selected);
  return index < 0 ? 0 : index;
}

/**
 * The parent row of `rows[index]`: the nearest earlier row one level up, or -1 at the top level.
 *
 * A flat list is what the tree renders (one `treeitem` per line, indented by `aria-level`), so "the
 * parent" is a search backwards for a row shallower than this one. Doing it over the *rendered* rows
 * rather than over the project is what makes ArrowLeft from a visible child land on its directory and
 * ArrowLeft from a hidden child unreachable.
 */
function parentIndex(rows: readonly TreeRow[], index: number): number {
  const level = rows[index].depth;
  if (level === 0) {
    return -1;
  }
  for (let cursor = index - 1; cursor >= 0; cursor -= 1) {
    if (rows[cursor].depth < level) {
      return cursor;
    }
  }
  return -1;
}

export function ProjectTree({
  rows,
  selected,
  dirty,
  onSelect,
  onToggleDir,
  toolbar,
}: {
  rows: TreeRow[];
  selected: string;
  /** The files with unsaved changes: a row for one carries the same dot a tab does. */
  dirty: readonly string[];
  onSelect: (row: TreeRow) => void;
  onToggleDir: (path: string) => void;
  /** The load/save controls, drawn between the header and the tree. */
  toolbar?: ReactNode;
}) {
  const container = useRef<HTMLDivElement | null>(null);

  // Moves both the selection and the focus to `rows[next]`. The items are read back out of the rendered
  // container rather than kept in a ref map: the tree is a few dozen rows, and a callback ref per row
  // would be more machinery than the move is worth.
  function focusRow(next: number): void {
    const row = rows[next];
    if (!row) {
      return;
    }
    onSelect(row);
    const items = container.current?.querySelectorAll<HTMLElement>('[role="treeitem"]');
    items?.[next]?.focus();
  }

  function onKeyDown(event: React.KeyboardEvent<HTMLDivElement>): void {
    const index = selectedIndex(rows, selected);
    const row = rows[index];
    if (!row) {
      return;
    }
    switch (event.key) {
      case "ArrowDown":
        event.preventDefault();
        focusRow(Math.min(index + 1, rows.length - 1));
        return;
      case "ArrowUp":
        event.preventDefault();
        focusRow(Math.max(index - 1, 0));
        return;
      case "Home":
        event.preventDefault();
        focusRow(0);
        return;
      case "End":
        event.preventDefault();
        focusRow(rows.length - 1);
        return;
      case "ArrowRight":
        event.preventDefault();
        // On a collapsed directory this opens it; on an open one it steps into the first child. That is
        // the WAI-ARIA tree pattern, and it is also what a file explorer does.
        if (row.dir && !row.expanded) {
          onToggleDir(row.path);
          return;
        }
        if (index + 1 < rows.length && rows[index + 1].depth > row.depth) {
          focusRow(index + 1);
        }
        return;
      case "ArrowLeft": {
        event.preventDefault();
        if (row.dir && row.expanded) {
          onToggleDir(row.path);
          return;
        }
        const parent = parentIndex(rows, index);
        if (parent >= 0) {
          focusRow(parent);
        }
        return;
      }
      case "Enter":
      case " ":
        event.preventDefault();
        if (row.dir) {
          onToggleDir(row.path);
        } else {
          onSelect(row);
        }
        return;
      default:
        return;
    }
  }

  return (
    <aside className="region region-tree" aria-labelledby="tree-heading">
      <div className="region-header">
        <h2 id="tree-heading">Project</h2>
        <span className="status" id="project-status" role="status" aria-live="polite">
          {rows.filter((row) => !row.dir).length} file(s)
        </span>
      </div>
      {toolbar}
      <div
        className="tree"
        id="project-tree"
        role="tree"
        aria-label="Project files"
        ref={container}
        onKeyDown={onKeyDown}
      >
        {rows.map((row) => {
          const isSelected = row.path === selected;
          return (
            <div
              key={row.path}
              role="treeitem"
              className={
                "tree-row" +
                (row.dir ? " is-dir" : "") +
                (row.editable ? "" : " is-locked") +
                (isSelected ? " is-selected" : "")
              }
              data-path={row.path}
              aria-level={row.depth + 1}
              aria-selected={isSelected}
              aria-expanded={row.dir ? row.expanded : undefined}
              tabIndex={isSelected ? 0 : -1}
              style={{ paddingLeft: `${8 + row.depth * 14}px` }}
              onClick={() => (row.dir ? onToggleDir(row.path) : onSelect(row))}
            >
              <span className="tree-glyph" aria-hidden="true">
                {row.dir ? (row.expanded ? "\u25be" : "\u25b8") : "\u00b7"}
              </span>
              <span className="tree-name">{row.name}</span>
              {!row.dir && dirty.includes(row.path) ? (
                <span className="tree-dirty" title="Unsaved changes" aria-label="Unsaved changes">
                  &bull;
                </span>
              ) : null}
              {!row.dir && !row.editable ? (
                <span className="tree-lock" title={row.viewOnly ? "Locked: opens read-only" : "Locked: not shown"}>
                  locked
                </span>
              ) : null}
            </div>
          );
        })}
        {rows.length === 0 ? <p className="tree-empty">This project has no files.</p> : null}
      </div>
    </aside>
  );
}
