// SPDX-License-Identifier: MIT

// The Problems panel (owner decision, studio focus pass; Visual Studio's Error List, VS Code's Problems): a dock
// panel listing the compiler's diagnostics for the project, each with a severity icon, its code, its message and
// `file:line:col`. The severity chips filter the list, and a row jumps to its span in the editor.
//
// It is a normal dock panel (`panels.json` id `problems`), so it moves, floats, splits and closes like any
// other tab (ADR-062). **A hidden panel does no work**: it subscribes to the store only while it is visible
// (the active tab of an expanded group), so a compile while the bottom dock is collapsed costs the panel
// nothing, and it renders its list the moment it is shown again.

import { useState, useSyncExternalStore, type KeyboardEvent } from "react";
import {
  EMPTY_SNAPSHOT,
  SEVERITIES,
  filterProblems,
  locationOf,
  type Problem,
  type ProblemSeverity,
  type ProblemsStore,
} from "../lib/problems";

/** The icon glyphs, text so they follow the colour tokens and need no asset; the label carries the meaning. */
export const SEVERITY_ICON: Readonly<Record<ProblemSeverity, string>> = { error: "⊗", warning: "⚠", info: "ⓘ" };
const SEVERITY_LABEL: Readonly<Record<ProblemSeverity, string>> = { error: "Error", warning: "Warning", info: "Info" };
const CHIP_LABEL: Readonly<Record<ProblemSeverity, string>> = { error: "Errors", warning: "Warnings", info: "Info" };

const noopSubscribe = (): (() => void) => () => {};

function moveFocus(event: KeyboardEvent<HTMLElement>): void {
  const key = event.key;
  if (key !== "ArrowDown" && key !== "ArrowUp" && key !== "Home" && key !== "End") {
    return;
  }
  const rows = Array.from(event.currentTarget.querySelectorAll<HTMLElement>("[data-problem]"));
  const index = rows.indexOf(document.activeElement as HTMLElement);
  if (rows.length === 0) {
    return;
  }
  event.preventDefault();
  const next = key === "Home" ? 0 : key === "End" ? rows.length - 1 : Math.min(Math.max(index + (key === "ArrowDown" ? 1 : -1), 0), rows.length - 1);
  rows[next].focus();
}

export function ProblemsPanel({
  store,
  visible,
  onJump,
}: {
  store: ProblemsStore;
  /** From the layout model's visible set; while `false` the panel neither subscribes nor renders rows. */
  visible: boolean;
  onJump: (problem: Problem) => void;
}) {
  const snapshot = useSyncExternalStore(visible ? store.subscribe : noopSubscribe, store.get, () => EMPTY_SNAPSHOT);
  const [shown, setShown] = useState<ReadonlySet<ProblemSeverity>>(() => new Set(SEVERITIES));
  if (!visible) {
    return <section className="problems" id="problems-panel" hidden />;
  }
  const rows = filterProblems(snapshot.problems, shown);
  const counts: Record<ProblemSeverity, number> = { error: snapshot.errors, warning: snapshot.warnings, info: snapshot.infos };
  const toggle = (severity: ProblemSeverity): void => {
    const next = new Set(shown);
    if (!next.delete(severity)) {
      next.add(severity);
    }
    setShown(next);
  };
  const empty = !snapshot.ran
    ? "Problems appear here once the project has compiled."
    : snapshot.problems.length === 0
      ? "No problems have been detected in the project."
      : "No problems match the filter.";
  return (
    <section className="problems" id="problems-panel" aria-label="Problems" tabIndex={-1}>
      <div className="problems-filters" role="group" aria-label="Filter problems by severity">
        {SEVERITIES.map((severity) => (
          <button
            key={severity}
            type="button"
            className={`problems-chip problems-${severity}`}
            data-problems-filter={severity}
            aria-pressed={shown.has(severity)}
            onClick={() => toggle(severity)}
          >
            <span aria-hidden="true">{SEVERITY_ICON[severity]}</span> {CHIP_LABEL[severity]} <span className="problems-count">{counts[severity]}</span>
          </button>
        ))}
      </div>
      {rows.length === 0 ? (
        <p className="problems-empty" role="status">
          {empty}
        </p>
      ) : (
        <ul className="problems-list" id="problems-list" aria-label="Problems list" onKeyDown={moveFocus}>
          {rows.map((problem) => (
            <li key={problem.key}>
              <button
                type="button"
                className={`problems-row problems-${problem.severity}`}
                data-problem={problem.key}
                data-severity={problem.severity}
                data-file={problem.file}
                data-line={problem.line}
                data-column={problem.column}
                title={`${SEVERITY_LABEL[problem.severity]} ${problem.code}: ${problem.message}`}
                onClick={() => onJump(problem)}
              >
                <span className="problems-icon" role="img" aria-label={SEVERITY_LABEL[problem.severity]}>
                  {SEVERITY_ICON[problem.severity]}
                </span>
                <span className="problems-code">{problem.code || "(no code)"}</span>
                <span className="problems-message">{problem.message}</span>
                <span className="problems-where">{locationOf(problem)}</span>
              </button>
            </li>
          ))}
        </ul>
      )}
    </section>
  );
}
