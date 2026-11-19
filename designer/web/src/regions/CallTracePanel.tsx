// SPDX-License-Identifier: MIT

// The call trace tab of the bottom dock (issue #434, `studio-mock-services-v1.md` sections 7 and 8): the
// recorded calls, a JSON editor for the selected call's response, and the trace actions.
//
// Applying an edit is explicit (`Apply`, never on keystroke, spec section 8.3), and a refused edit shows
// the section 10 code instead of a raw exception. `delayMs` is shown as recorded metadata: the service
// call path is synchronous in v1, so the console must not present it as a live delay.

import { useState } from "react";
import type { CallEntry, MockConsole } from "../lib/mock-console";

function argsLabel(args: unknown[]): string {
  const text = JSON.stringify(args);
  return text.length > 48 ? `${text.slice(0, 47)}…` : text;
}

function EntryEditor({ entry, mocks }: { entry: CallEntry; mocks: MockConsole }) {
  const initial = JSON.stringify(entry.response ?? {}, null, 2);
  const [text, setText] = useState(initial);
  const [problem, setProblem] = useState("");
  return (
    <form
      className="trace-editor"
      aria-label={`Response of call ${entry.seq}`}
      onSubmit={(event) => {
        event.preventDefault();
        setProblem(mocks.editResponse(entry.seq, text) ?? "");
      }}
    >
      <label htmlFor="trace-response">
        Response for #{entry.seq} {entry.interface}.{entry.method}
      </label>
      <textarea
        id="trace-response"
        spellCheck={false}
        value={text}
        onChange={(event) => setText(event.currentTarget.value)}
        aria-invalid={problem !== ""}
        aria-describedby="trace-problem"
        rows={5}
      />
      <div className="trace-editor-actions">
        <button type="submit" id="trace-apply">
          Apply
        </button>
        <span className="dock-hint">
          The first frame of a new call uses the engine default; an applied edit is used by the next frame.
        </span>
      </div>
      <p id="trace-problem" className="dock-error" role="alert">
        {problem}
      </p>
    </form>
  );
}

export function CallTracePanel({ mocks }: { mocks: MockConsole }) {
  const [selected, setSelected] = useState<number | null>(null);
  const chosen = mocks.entries.find((entry) => entry.seq === selected) ?? null;
  return (
    <div className="dock-body" id="call-trace">
      <div className="trace-split">
      {mocks.entries.length === 0 ? (
        <p className="dock-empty" id="trace-empty">
          No calls recorded yet. A call appears here when the preview&rsquo;s service fake reports it. That
          path does not exist yet (ADR-055 decision 9), so nothing is recorded in this build.
        </p>
      ) : (
        <div className="trace-table-wrap">
          <table className="trace-table" aria-label="Recorded service calls">
            <thead>
              <tr>
                <th scope="col">#</th>
                <th scope="col">Call</th>
                <th scope="col">Args</th>
                <th scope="col">Response</th>
                <th scope="col">Sim time</th>
              </tr>
            </thead>
            <tbody>
              {mocks.entries.map((entry) => (
                <tr
                  key={entry.seq}
                  data-state={entry.state}
                  aria-selected={entry.seq === selected}
                  className={entry.seq === selected ? "trace-selected" : undefined}
                >
                  <td>
                    <button type="button" className="trace-pick" onClick={() => setSelected(entry.seq)}>
                      {entry.seq}
                    </button>
                  </td>
                  <td>
                    {entry.interface}.{entry.method}
                  </td>
                  <td className="trace-args">{argsLabel(entry.args)}</td>
                  <td>
                    <span className={`trace-state trace-state-${entry.state}`}>{entry.state}</span>
                    {entry.delayMs !== null ? (
                      <span className="dock-hint" title="Recorded metadata: the v1 service call path is synchronous">
                        {" "}
                        delayMs {entry.delayMs} (recorded, not a live delay)
                      </span>
                    ) : null}
                  </td>
                  <td>{entry.simTimeMs} ms</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
      {chosen !== null ? <EntryEditor key={`${chosen.seq}-${chosen.state}`} entry={chosen} mocks={mocks} /> : null}
      </div>
      {mocks.traceStopped !== null ? (
        <p className="dock-error" role="alert">
          {mocks.traceStopped}: the trace is full, so later calls are not recorded.
        </p>
      ) : null}
      <div className="dock-actions">
        <button type="button" id="trace-clear" onClick={mocks.clearTrace} disabled={mocks.entries.length === 0}>
          Clear trace
        </button>
        <button type="button" id="trace-download" onClick={mocks.downloadTrace} disabled={mocks.entries.length === 0}>
          Download trace
        </button>
      </div>
    </div>
  );
}
