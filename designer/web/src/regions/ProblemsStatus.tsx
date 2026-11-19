// SPDX-License-Identifier: MIT

// The status bar's Problems item: the error and warning counts with separate icons (VS Code's `⊗ 0  ⚠ 0`),
// derived from the Problems store, which the diagnostics session fills from its diagnostics array. A click runs
// `view.problems`, which opens or focuses the panel. This component is the only part of the bar that re-renders
// on a compile; the page does not.

import { useSyncExternalStore } from "react";
import { EMPTY_SNAPSHOT, summaryOf, type ProblemsStore } from "../lib/problems";
import type { Workbench } from "../page-commands";
import { SEVERITY_ICON } from "./ProblemsPanel";

export function ProblemsStatus({
  workbench,
  store,
  open,
}: {
  workbench: Workbench;
  store: ProblemsStore;
  /** The Problems panel is the visible tab. */
  open: boolean;
}) {
  const snapshot = useSyncExternalStore(store.subscribe, store.get, () => EMPTY_SNAPSHOT);
  const value = (n: number): string => (snapshot.ran ? String(n) : "–");
  return (
    <button
      type="button"
      className="status-item status-toggle"
      id="status-diagnostics-toggle"
      data-command="view.problems"
      aria-pressed={open}
      aria-label={`${summaryOf(snapshot)}. Open the Problems panel`}
      title={`${summaryOf(snapshot)}. Open the Problems panel (Ctrl+Shift+M)`}
      onClick={() => void workbench.run("view.problems")}
    >
      <span className="status-problem" id="status-errors" data-count={snapshot.errors}>
        <span aria-hidden="true">{SEVERITY_ICON.error}</span> <span className="status-problem-count">{value(snapshot.errors)}</span>
      </span>{" "}
      <span className="status-problem" id="status-warnings" data-count={snapshot.warnings}>
        <span aria-hidden="true">{SEVERITY_ICON.warning}</span> <span className="status-problem-count">{value(snapshot.warnings)}</span>
      </span>
    </button>
  );
}
