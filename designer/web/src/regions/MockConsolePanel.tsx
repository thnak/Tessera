// SPDX-License-Identifier: MIT

// The mock console tab of the bottom dock (issue #434, `studio-mock-services-v1.md` section 8): the list
// of mocks with their binding status and reason, their diagnostics, and the session actions.
//
// It does not run mock code and never shows a raw exception: every failure is a `TESSERA_MOCK_*` code
// with its file and position where the host has one (spec section 8, last paragraph).

import type { MockConsole, MockDiagnostic, MockStatus } from "../lib/mock-console";

function position(diagnostic: MockDiagnostic): string {
  if (diagnostic.file === undefined) {
    return "";
  }
  return diagnostic.line === undefined
    ? diagnostic.file
    : `${diagnostic.file}:${diagnostic.line}:${diagnostic.column ?? 1}`;
}

function MockRow({ mock, onRestart }: { mock: MockStatus; onRestart: (path: string) => void }) {
  return (
    <li className="mock-row" data-status={mock.status}>
      <div className="mock-row-head">
        <span className="mock-path">{mock.path}</span>
        <span className={`mock-badge mock-badge-${mock.status}`}>{mock.status}</span>
        {mock.interface !== "" ? <code className="mock-iface">{mock.interface}</code> : null}
        {mock.disabled ? (
          <button type="button" className="mock-restart" onClick={() => onRestart(mock.path)}>
            Restart worker
          </button>
        ) : null}
      </div>
      {mock.reason !== "" ? <p className="mock-reason">{mock.reason}</p> : null}
      {mock.diagnostics.length > 0 ? (
        <ul className="mock-diagnostics" aria-label={`Diagnostics for ${mock.path}`}>
          {mock.diagnostics.map((diagnostic, index) => (
            <li key={`${diagnostic.code}-${index}`} data-severity={diagnostic.severity}>
              <span className="mock-diagnostic-code">{diagnostic.code}</span>{" "}
              {position(diagnostic) !== "" ? <span className="mock-diagnostic-at">{position(diagnostic)}</span> : null}{" "}
              {diagnostic.message}
            </li>
          ))}
        </ul>
      ) : null}
    </li>
  );
}

export function MockConsolePanel({ mocks, hasMockFiles }: { mocks: MockConsole; hasMockFiles: boolean }) {
  const manifestCount = mocks.manifest === null ? 0 : Object.keys(mocks.manifest.interfaces).length;
  return (
    <div className="dock-body" id="mock-console">
      <p className="dock-note" role="note">
        The engine has no service seam yet (ADR-055 decision 9), so a mock cannot change a previewed
        frame. Responses here are recorded and scripted, not applied to the preview.
      </p>
      <p className="dock-manifest">
        {mocks.manifest === null
          ? "Service manifest: none. ui/services.json has no spec yet, so no interface can bind and every mock is unbound."
          : `Service manifest: ${manifestCount} interface${manifestCount === 1 ? "" : "s"}.`}
      </p>
      {!hasMockFiles ? (
        <p className="dock-empty" id="mock-empty">
          This project has no mock files. Add a <code>*.mock.ts</code> file, for example{" "}
          <code>mocks/navigator.mock.ts</code>, and it is compiled here. Nothing is downloaded until you do.
        </p>
      ) : mocks.phase === "loading" ? (
        <p className="dock-empty" role="status">
          Loading the TypeScript transpiler and starting the mock sandbox...
        </p>
      ) : mocks.phase === "failed" ? (
        <p className="dock-empty dock-error" role="alert">
          The mock host could not start: {mocks.failure}
        </p>
      ) : (
        <>
          <div className="dock-actions">
            <button type="button" id="mock-reset" onClick={mocks.reset}>
              Reset session
            </button>
            <button type="button" id="mock-toggle" aria-pressed={!mocks.enabled} onClick={() => mocks.setEnabled(!mocks.enabled)}>
              {mocks.enabled ? "Disable mocks" : "Enable mocks"}
            </button>
          </div>
          <ul className="mock-list" aria-label="Mock files">
            {mocks.statuses.map((mock) => (
              <MockRow key={mock.path} mock={mock} onRestart={mocks.restart} />
            ))}
          </ul>
        </>
      )}
    </div>
  );
}
