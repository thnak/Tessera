// SPDX-License-Identifier: MIT

// The project's status block (issue #391, D-S4), at the top of the left column: what is open, the line saying
// what the last action did, the stored-draft offer and -- only when a save found a file that changed on disk --
// the conflict choice. The Open, Save, Download and file-operation buttons that used to sit here are File menu
// commands now (issue #557); the logic is `page-project-io.ts`, this draws what it says.

import type { ProjectIo } from "../page-project-io";

export function ProjectBar({ io }: { io: ProjectIo }) {
  const { state } = io;
  return (
    <div className="project-bar">
      {state.name !== "" ? (
        <p className="project-name" id="project-name">
          {state.source === "folder" ? "Folder" : state.source === "sample" ? "Sample" : "Zip"}: {state.name}
          {state.source === "zip" ? " (a copy: Save needs an opened folder, use Download)" : ""}
        </p>
      ) : null}
      <p className="project-note" id="project-note" hidden={state.note === ""}>
        {state.note}
      </p>
      {state.draft !== null ? (
        <div className="project-conflict" id="project-draft" role="alertdialog" aria-label="Unsaved draft">
          <p>
            Unsaved changes to {state.draft.files} file(s) from {new Date(state.draft.savedAt).toLocaleString()} were
            kept in this browser.
          </p>
          <div className="project-actions">
            <button type="button" id="restore-draft" onClick={io.restoreDraft}>
              Restore draft
            </button>
            <button type="button" id="discard-draft" onClick={io.discardDraft}>
              Discard
            </button>
          </div>
        </div>
      ) : null}
      {state.conflicts.length > 0 ? (
        <div className="project-conflict" id="project-conflict" role="alertdialog" aria-label="Changed on disk">
          <p>Changed on disk: {state.conflicts.join(", ")}</p>
          <div className="project-actions">
            <button type="button" onClick={() => io.resolveConflicts("keep-mine")}>
              Keep mine
            </button>
            <button type="button" onClick={() => io.resolveConflicts("take-disk")}>
              Take disk
            </button>
            <button type="button" onClick={() => io.resolveConflicts("cancel")}>
              Cancel
            </button>
          </div>
        </div>
      ) : null}
    </div>
  );
}
