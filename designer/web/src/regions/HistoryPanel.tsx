// SPDX-License-Identifier: MIT

// The Version History panel (#583, `view.history`): a dock panel in the side bar group next to Search. It lists
// the active file's local snapshots newest first (why it was taken, how long ago, how far it is from the working
// text), shows the changed lines of the selected one, and restores it. Restore is an editor edit, so Undo
// (Ctrl+Z in the editor) takes it back in one step. Nothing here is uploaded: versions live in this browser.
//
// Keyboard: Arrow keys, Home and End move between versions; Enter or Space selects one; the Restore button
// (or Ctrl+Enter on a selected version) restores it. Outcomes are announced through one polite live region.

import type { KeyboardEvent, ReactNode } from "react";
import type { HistoryHandle } from "../page-history";

function move(event: KeyboardEvent<HTMLElement>): void {
  const key = event.key;
  if (key !== "ArrowDown" && key !== "ArrowUp" && key !== "Home" && key !== "End") {
    return;
  }
  const rows = Array.from(event.currentTarget.querySelectorAll<HTMLElement>("[data-history-row]"));
  if (rows.length === 0) {
    return;
  }
  event.preventDefault();
  const index = rows.indexOf(document.activeElement as HTMLElement);
  const next = key === "Home" ? 0 : key === "End" ? rows.length - 1 : Math.min(Math.max(index + (key === "ArrowDown" ? 1 : -1), 0), rows.length - 1);
  rows[next].focus();
}

export function HistoryPanel({ history, visible }: { history: HistoryHandle; visible: boolean }): ReactNode {
  if (!visible) {
    return <section className="history" id="history-panel" hidden />;
  }
  const { rows, selected, diff } = history;
  return (
    <section className="history" id="history-panel" aria-label="Version history" onKeyDown={move}>
      <header className="history-head">
        <span className="history-path" title={history.path}>{history.path === "" ? "No file open" : history.path}</span>
      </header>
      <p className="history-status" role="status" aria-live="polite">
        {history.notice !== "" ? history.notice : rows.length === 0 ? "No saved versions of this file yet. A version is kept on every save." : `${rows.length} version${rows.length === 1 ? "" : "s"}, kept in this browser only.`}
      </p>
      <ul className="history-list" role="listbox" aria-label="Versions">
        {rows.map((row) => (
          <li key={row.id} role="option" aria-selected={row.id === selected}>
            <button
              type="button"
              className={row.id === selected ? "history-row selected" : "history-row"}
              data-history-row
              onClick={() => history.select(row.id === selected ? null : row.id)}
              onKeyDown={(event) => {
                if ((event.ctrlKey || event.metaKey) && event.key === "Enter") {
                  event.preventDefault();
                  history.restore(row.id);
                }
              }}
            >
              <span className="history-label">{row.label}</span>
              <span className="history-age">{row.age}</span>
              <span className="history-stat">{row.current ? "current" : `+${row.added} -${row.removed}`}</span>
            </button>
          </li>
        ))}
      </ul>
      {selected !== null && diff !== null ? (
        <div className="history-detail">
          <button type="button" className="history-restore" onClick={() => history.restore(selected)} disabled={rows.find((row) => row.id === selected)?.current === true}>
            Restore this version
          </button>
          <pre className="history-diff" aria-label="Changes since this version">
            {diff.rows.length === 0 ? "Same as the current text." : null}
            {diff.rows.map((line, index) => (
              <span key={index} className={line.kind === "insert" ? "history-add" : "history-del"}>
                {line.kind === "insert" ? "+ " : "- "}
                {line.text}
                {"\n"}
              </span>
            ))}
            {diff.hidden > 0 ? `... ${diff.hidden} more changed lines not shown` : null}
          </pre>
        </div>
      ) : null}
    </section>
  );
}
