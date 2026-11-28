// SPDX-License-Identifier: MIT

// The keyboard-shortcuts pane of the settings dialog (slice W8 #565): every command with its chord, where the chord
// comes from, the commands it collides with, an inline editor that records a chord, and JSON import and export of
// the overrides. The rules live in `workbench/keybindings.js`; the edits go through `page-settings.ts`.

import { useState } from "react";
import type { KeybindingRow } from "../workbench/index.js";
import type { KeybindingTools, Outcome } from "../page-settings";
import { JsonTransfer } from "./JsonTransfer";

const SOURCE_LABELS: Record<KeybindingRow["source"], string> = { default: "Default", user: "Yours", removed: "Removed", unbound: "None" };

/** A key event that should be recorded as a chord instead of typed: a modifier combination or a function key. */
function isChordEvent(event: React.KeyboardEvent): boolean {
  return event.ctrlKey || event.altKey || event.metaKey || /^F\d{1,2}$/.test(event.key);
}

function Editor({ row, tools, onDone }: { row: KeybindingRow; tools: KeybindingTools; onDone: (outcome: Outcome | null) => void }) {
  const [draft, setDraft] = useState("");
  const [problem, setProblem] = useState("");
  const apply = () => {
    const result = tools.rebind(row.id, draft);
    if (!result.ok) {
      setProblem(result.message);
      return;
    }
    const also = result.collisions ?? [];
    onDone({
      ok: true,
      message: also.length === 0 ? result.message : `${result.message} It is also used by ${also.map(tools.title).join(", ")}.`,
    });
  };
  return (
    <div className="kb-editor">
      <input
        type="text"
        className="kb-input"
        autoFocus
        aria-label={`New shortcut for ${row.title}`}
        aria-invalid={problem !== ""}
        aria-describedby={problem === "" ? undefined : `kb-problem-${row.id}`}
        placeholder="Press the keys, or type Mod+Shift+L"
        value={draft}
        spellCheck={false}
        onChange={(event) => {
          setDraft(event.target.value);
          setProblem("");
        }}
        onKeyDown={(event) => {
          if (event.key === "Escape") {
            event.preventDefault();
            event.stopPropagation();
            onDone(null);
          } else if (event.key === "Enter" && !isChordEvent(event)) {
            event.preventDefault();
            apply();
          } else if (isChordEvent(event)) {
            // The editor's own text keys stay typeable; a chord is recorded, and the page's keymap never sees it.
            event.preventDefault();
            event.stopPropagation();
            const chord = tools.record(event.nativeEvent);
            if (chord !== null) {
              setDraft(chord);
              setProblem("");
            }
          }
        }}
      />
      <button type="button" className="primary" disabled={draft.trim() === ""} onClick={apply}>
        Set
      </button>
      <button type="button" onClick={() => onDone(tools.clear(row.id))}>
        Remove
      </button>
      <button type="button" onClick={() => onDone(null)}>
        Cancel
      </button>
      {problem === "" ? null : (
        <p className="settings-outcome is-error" id={`kb-problem-${row.id}`} role="alert">
          {problem}
        </p>
      )}
    </div>
  );
}

export function KeybindingsPane({ tools, onChanged }: { tools: KeybindingTools; onChanged: () => void }) {
  const [filter, setFilter] = useState("");
  const [editing, setEditing] = useState<string | null>(null);
  const [outcome, setOutcome] = useState<Outcome | null>(null);
  const needle = filter.trim().toLowerCase();
  const all = tools.rows();
  const rows = all.filter((row) => needle === "" || `${row.title} ${row.category} ${row.chord} ${row.id}`.toLowerCase().includes(needle));
  const conflicts = all.filter((row) => row.conflicts.length > 0).length;
  /** Closes the editor and puts focus back on the row's Change button (the editor's own field is gone). */
  const done = (next: Outcome | null, id?: string) => {
    setEditing(null);
    if (id !== undefined) {
      requestAnimationFrame(() => document.querySelector<HTMLElement>(`[data-command="${id}"] .kb-change`)?.focus());
    }
    if (next !== null) {
      setOutcome(next);
    }
    onChanged();
  };

  return (
    <div className="settings-pane" id="settings-keybindings">
      <div className="settings-row">
        <input
          type="search"
          id="kb-filter"
          className="kb-filter"
          aria-label="Filter shortcuts"
          placeholder="Filter by command or key"
          value={filter}
          onChange={(event) => setFilter(event.target.value)}
        />
        <button type="button" id="kb-reset-all" onClick={() => done(tools.resetAll())}>
          Reset all
        </button>
      </div>
      <p className="settings-note" id="kb-summary" role="status">
        {conflicts === 0 ? "No shortcut conflicts." : `${conflicts} commands share a shortcut with another command in the same scope.`}
      </p>
      <div className="kb-scroll">
        <table className="kb-table" id="kb-table">
          <thead>
            <tr>
              <th scope="col">Command</th>
              <th scope="col">Shortcut</th>
              <th scope="col">Source</th>
              <th scope="col">
                <span className="visually-hidden">Actions</span>
              </th>
            </tr>
          </thead>
          <tbody>
            {rows.map((row) => (
              <tr key={row.id} data-command={row.id} className={row.conflicts.length > 0 ? "is-conflict" : undefined}>
                <td>
                  <span className="kb-title">{row.title}</span> <span className="kb-category">{row.category}</span>
                </td>
                <td>
                  {editing === row.id ? (
                    <Editor row={row} tools={tools} onDone={(next) => done(next, row.id)} />
                  ) : (
                    <>
                      <kbd className="kb-chord">{row.chord === "" ? "Unbound" : row.chord}</kbd>
                      {row.conflicts.length === 0 ? null : (
                        <span className="kb-conflict">Conflicts with {row.conflicts.map(tools.title).join(", ")}</span>
                      )}
                      {row.shared.length === 0 ? null : <span className="kb-shared">Also in another scope: {row.shared.map(tools.title).join(", ")}</span>}
                    </>
                  )}
                </td>
                <td>{SOURCE_LABELS[row.source]}</td>
                <td className="kb-actions">
                  <button type="button" className="kb-change" aria-label={`Change shortcut for ${row.title}`} onClick={() => setEditing(row.id)}>
                    Change
                  </button>
                  {row.source === "user" || row.source === "removed" ? (
                    <button type="button" aria-label={`Reset shortcut for ${row.title}`} onClick={() => done(tools.reset(row.id), row.id)}>
                      Reset
                    </button>
                  ) : null}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
      <p className={outcome !== null && !outcome.ok ? "settings-outcome is-error" : "settings-outcome"} id="kb-outcome" role="status">
        {outcome?.message ?? ""}
      </p>
      <h3 className="settings-heading">Import or export</h3>
      <JsonTransfer kind="keybindings" fileName="tessera-keybindings.json" exportText={tools.exportText} onImport={tools.importText} onChanged={onChanged} />
    </div>
  );
}
