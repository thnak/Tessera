// SPDX-License-Identifier: MIT

// The bottom dock of the workspace (issue #434; "Tessera Studio Plan": Platform events, Mock console,
// Call trace). This slice owns the two ADR-055 tabs; a third tab can be added by whoever lands platform
// events without touching these.
//
// **Collapsed by default, and free until opened.** The dock is one tab strip. The mock host, the
// sandbox worker and the transpiler start only when the dock is open *and* the project has a mock file
// (spec section 2.4), so a project without mocks pays nothing. The page lays the dock out as a fixed-height
// strip under the workspace rather than inside the grid, so it does not change the three-column shape.

import { useId, useState, type KeyboardEvent } from "react";
import { useMockConsole } from "../lib/mock-console";
import type { ProjectFile } from "../lib/project";
import { CallTracePanel } from "./CallTracePanel";
import { MockConsolePanel } from "./MockConsolePanel";

type DockTab = "console" | "trace";
const TABS: readonly DockTab[] = ["console", "trace"];

export function MockDock({
  files,
  textOf,
  revision,
}: {
  files: readonly ProjectFile[];
  textOf: (path: string) => string;
  /** Bumped when a mock buffer is edited, so the sandbox worker is recreated (spec section 4.3). */
  revision: number;
}) {
  const [open, setOpen] = useState(false);
  const [tab, setTab] = useState<DockTab>("console");
  const ids = useId();
  const mocks = useMockConsole(open, files, textOf, revision);
  const mockFiles = files.filter((file) => file.kind === "mock");

  const select = (next: DockTab) => {
    setTab(next);
    setOpen(true);
  };
  const onKeyDown = (event: KeyboardEvent<HTMLButtonElement>) => {
    const index = TABS.indexOf(tab);
    if (event.key === "ArrowRight" || event.key === "ArrowLeft") {
      event.preventDefault();
      const next = TABS[(index + (event.key === "ArrowRight" ? 1 : TABS.length - 1)) % TABS.length];
      select(next);
      document.getElementById(`${ids}-tab-${next}`)?.focus();
    }
  };
  const errors = mocks.statuses.filter((mock) => mock.status === "error").length;

  return (
    <section className="dock" id="dock" aria-label="Mock services">
      <div className="dock-tabs" role="tablist" aria-label="Dock panels">
        <button
          type="button"
          role="tab"
          id={`${ids}-tab-console`}
          aria-selected={tab === "console"}
          aria-controls={`${ids}-panel`}
          tabIndex={tab === "console" ? 0 : -1}
          onClick={() => select("console")}
          onKeyDown={onKeyDown}
        >
          Mock console
          {mockFiles.length > 0 ? (
            <span className={errors > 0 ? "dock-count dock-count-error" : "dock-count"}>{mockFiles.length}</span>
          ) : null}
        </button>
        <button
          type="button"
          role="tab"
          id={`${ids}-tab-trace`}
          aria-selected={tab === "trace"}
          aria-controls={`${ids}-panel`}
          tabIndex={tab === "trace" ? 0 : -1}
          onClick={() => select("trace")}
          onKeyDown={onKeyDown}
        >
          Call trace
          {mocks.entries.length > 0 ? <span className="dock-count">{mocks.entries.length}</span> : null}
        </button>
        <button
          type="button"
          className="dock-toggle"
          id="dock-toggle"
          aria-expanded={open}
          aria-controls={`${ids}-panel`}
          onClick={() => setOpen((value) => !value)}
        >
          {open ? "Hide" : "Show"}
        </button>
      </div>
      {open ? (
        <div className="dock-panel" role="tabpanel" id={`${ids}-panel`} aria-labelledby={`${ids}-tab-${tab}`}>
          {tab === "console" ? (
            <MockConsolePanel mocks={mocks} hasMockFiles={mockFiles.length > 0} />
          ) : (
            <CallTracePanel mocks={mocks} />
          )}
        </div>
      ) : null}
    </section>
  );
}
