// SPDX-License-Identifier: MIT

// The **Resources** section of the Platform events tab (issue #657; `designer-protocol-v1.md` section 5.2).
//
// One row per fake resource of the open component: its status and epoch as the engine reports them, a
// value field, an epoch field, and the three controls -- Fetch (a refresh or a key change starts), Resolve
// (a completion lands with the value) and Fail (a completion fails with the value as its message). Under
// the rows, one sentence says whether a boundary shows its fallback or holds its content and what
// `isPending` is. A resolve with an epoch that is not the current one is a stale completion: the engine
// drops it and the panel says so, which is how a developer sees the epoch rule rather than reading it.
//
// The panel keeps only what the person typed; every fact it shows comes from the last report.

import { useState } from "react";
import {
  describePending,
  describeResource,
  draftError,
  requestFor,
  type ResourceAction,
  type ResourceDraft,
} from "../lib/resource-controls";
import type { ResourceControls } from "../page-resources";

const EMPTY_DRAFT: ResourceDraft = { value: "", epoch: "" };

export function ResourcePanel({ resources }: { resources: ResourceControls }) {
  const [drafts, setDrafts] = useState<Record<string, ResourceDraft>>({});
  const [notice, setNotice] = useState<string | null>(null);
  const report = resources.report;
  if (report === null || (report.resources.length === 0 && report.error === "")) {
    // No app yet, or a component with no `useResource`: nothing to control, and no panel noise.
    return null;
  }

  const draftOf = (id: string): ResourceDraft => drafts[id] ?? EMPTY_DRAFT;
  const edit = (id: string, patch: Partial<ResourceDraft>) =>
    setDrafts((previous) => ({ ...previous, [id]: { ...draftOf(id), ...patch } }));

  function press(action: Exclude<ResourceAction, "status">, id: string) {
    const draft = draftOf(id);
    const problem = draftError(draft);
    if (problem !== null) {
      setNotice(problem);
      return;
    }
    const reply = resources.send(requestFor(action, id, draft));
    if (!reply.ok) {
      setNotice(reply.error);
    } else if (!reply.accepted) {
      setNotice(`The ${action} for epoch ${draft.epoch === "" ? "(current)" : draft.epoch} was dropped: it is not the current epoch.`);
    } else {
      setNotice(null);
    }
  }

  return (
    <section className="resource-panel" id="resource-panel" aria-labelledby="resource-heading">
      <div className="log-head">
        <span id="resource-heading">Resources (fake, deterministic)</span>
        <span className="log-actions">
          <button type="button" id="resource-refresh" onClick={resources.refresh}>
            Refresh
          </button>
        </span>
      </div>
      {report.resources.length === 0 ? (
        <p className="dock-empty" id="resource-error" role="status">
          {report.error}
        </p>
      ) : (
        <>
          <ul className="resource-rows" id="resource-rows">
            {report.resources.map((row) => {
              const draft = draftOf(row.id);
              return (
                <li key={row.id} className="resource-row" data-resource={row.id} data-status={row.status}>
                  <span className="resource-id">{row.id}</span>
                  <span className="resource-status" role="status" aria-live="polite">
                    {describeResource(row)}
                  </span>
                  <input
                    type="text"
                    className="resource-value"
                    aria-label={`Value for ${row.id}`}
                    placeholder="value or message"
                    value={draft.value}
                    onChange={(event) => edit(row.id, { value: event.currentTarget.value })}
                  />
                  <input
                    type="text"
                    inputMode="numeric"
                    className="resource-epoch"
                    aria-label={`Epoch for ${row.id}`}
                    placeholder="epoch (current)"
                    value={draft.epoch}
                    onChange={(event) => edit(row.id, { epoch: event.currentTarget.value })}
                  />
                  <button type="button" data-action="fetch" onClick={() => press("fetch", row.id)}>
                    Fetch
                  </button>
                  <button type="button" data-action="resolve" onClick={() => press("resolve", row.id)}>
                    Resolve
                  </button>
                  <button type="button" data-action="fail" onClick={() => press("fail", row.id)}>
                    Fail
                  </button>
                </li>
              );
            })}
          </ul>
          <p className="resource-summary" id="resource-summary" role="status" aria-live="polite">
            {describePending(report)}
          </p>
        </>
      )}
      <p className="log-notice" id="resource-notice" role="status" hidden={notice === null}>
        {notice}
      </p>
    </section>
  );
}
