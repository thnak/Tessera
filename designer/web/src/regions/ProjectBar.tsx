// SPDX-License-Identifier: MIT

// The project's load and save controls (issue #391, D-S4), at the top of the left column: Open folder,
// Open zip, Save and Download project, the line saying what the last action did, and -- only when a save
// found a file that changed on disk -- the conflict choice. The logic is `page-project-io.ts`; this draws it.

import { useRef } from "react";
import type { ProjectIo } from "../page-project-io";

export function ProjectBar({ io, dirtyCount }: { io: ProjectIo; dirtyCount: number }) {
  const zipInput = useRef<HTMLInputElement | null>(null);
  const { state } = io;
  const canSave = state.source === "folder" && !state.busy;
  return (
    <div className="project-bar">
      <div className="project-actions" role="toolbar" aria-label="Project files">
        <button
          type="button"
          id="open-folder"
          onClick={io.openFolder}
          disabled={state.busy || !io.folderSupported}
          title={
            io.folderSupported
              ? "Open a project folder from this computer"
              : "This browser cannot open a folder; use Open zip"
          }
        >
          Open folder
        </button>
        <button type="button" id="open-zip" onClick={() => zipInput.current?.click()} disabled={state.busy}>
          Open zip
        </button>
        <input
          ref={zipInput}
          type="file"
          accept=".zip,application/zip"
          hidden
          aria-label="Project zip"
          onChange={(event) => {
            const file = event.currentTarget.files?.[0];
            event.currentTarget.value = "";
            if (file) {
              io.openZip(file);
            }
          }}
        />
        <button
          type="button"
          id="save-project"
          onClick={io.save}
          disabled={!canSave}
          title={
            state.source === "folder"
              ? "Write the changed files back to the folder (Ctrl+S)"
              : "Open a folder to save into it; otherwise use Download project"
          }
        >
          Save{dirtyCount > 0 ? ` (${dirtyCount})` : ""}
        </button>
        <button type="button" id="download-project" onClick={io.download} disabled={state.busy}>
          Download
        </button>
      </div>
      {io.recents.length > 0 && io.folderSupported ? (
        <div className="project-actions project-recent" role="group" aria-label="Recent projects">
          <select
            id="recent-projects"
            aria-label="Recent projects"
            value=""
            disabled={state.busy}
            title="Projects opened before; the browser asks for folder access again"
            onChange={(event) => {
              const index = Number(event.currentTarget.value);
              if (Number.isInteger(index)) {
                io.reopenRecent(index);
              }
            }}
          >
            <option value="">Recent projects ({io.recents.length})</option>
            {io.recents.map((recent, index) => (
              <option key={`${recent.name}:${index}`} value={String(index)}>
                {recent.name}
                {recent.openedAt > 0 ? ` - ${new Date(recent.openedAt).toLocaleDateString()}` : ""}
              </option>
            ))}
          </select>
          <button type="button" id="forget-recent" onClick={io.forgetRecents} disabled={state.busy}>
            Forget
          </button>
        </div>
      ) : null}
      <div className="project-actions" role="toolbar" aria-label="File operations">
        <button type="button" id="new-file" onClick={io.newFile} disabled={state.busy}>
          New file
        </button>
        <button type="button" id="rename-file" onClick={io.renameFile} disabled={state.busy}>
          Rename
        </button>
        <button type="button" id="delete-file" onClick={io.deleteFile} disabled={state.busy}>
          Delete
        </button>
      </div>
      {state.name !== "" ? (
        <p className="project-name" id="project-name">
          {state.source === "folder" ? "Folder" : "Zip"}: {state.name}
          {state.source === "zip" ? " (a copy: Save needs an opened folder, use Download)" : ""}
        </p>
      ) : null}
      <p className="project-note" id="project-note" role="status" aria-live="polite" hidden={state.note === ""}>
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
