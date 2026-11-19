// SPDX-License-Identifier: MIT

// The bottom dock of the workspace ("Tessera Studio Plan": Platform events, Mock console, Call trace).
// One tab strip and one panel hold the three tabs: the Platform events panel (issue #448, device
// profiles) and the two ADR-055 mock tabs (issue #434).
//
// **Open on Platform events by default.** The dock was always visible for the platform tab, so it stays
// open. The mock host, the sandbox worker and the transpiler still start only when the dock is open *and*
// the project has a mock file (spec section 2.4), so a project without mocks pays nothing. The page lays
// the dock out as a strip under the workspace rather than inside the grid, so it does not change the
// three-column shape; its height is capped (`.dock`), and it can be hidden to give the canvas the room.

import { useId, useState, type KeyboardEvent } from "react";
import { useMockConsole } from "../lib/mock-console";
import type { DeviceProfile } from "../lib/device-profile";
import type { ProjectFile } from "../lib/project";
import type { PlatformControls } from "../page-platform";
import { CallTracePanel } from "./CallTracePanel";
import { MockConsolePanel } from "./MockConsolePanel";
import { PlatformPanel } from "./PlatformDock";

type DockTab = "platform" | "console" | "trace";
const TABS: readonly DockTab[] = ["platform", "console", "trace"];

export function MockDock({
  files,
  textOf,
  revision,
  profile,
  platform,
  onRestart,
}: {
  files: readonly ProjectFile[];
  textOf: (path: string) => string;
  /** Bumped when a mock buffer is edited, so the sandbox worker is recreated (spec section 4.3). */
  revision: number;
  /** The profile in force (the custom size's own profile when none is picked). */
  profile: DeviceProfile | null;
  platform: PlatformControls;
  /** The **Restart app** action: a cold start, state reset. */
  onRestart: () => void;
}) {
  const [open, setOpen] = useState(true);
  const [tab, setTab] = useState<DockTab>("platform");
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
    <section className="dock" id="dock" aria-label="Bottom dock">
      <div className="dock-tabs" role="tablist" aria-label="Dock panels">
        <button
          type="button"
          role="tab"
          id={`${ids}-tab-platform`}
          aria-selected={tab === "platform"}
          aria-controls={`${ids}-panel`}
          tabIndex={tab === "platform" ? 0 : -1}
          onClick={() => select("platform")}
          onKeyDown={onKeyDown}
        >
          Platform events
          {platform.log.length > 0 ? <span className="dock-count">{platform.log.length}</span> : null}
        </button>
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
          {tab === "platform" ? (
            <PlatformPanel profile={profile} platform={platform} onRestart={onRestart} />
          ) : tab === "console" ? (
            <MockConsolePanel mocks={mocks} hasMockFiles={mockFiles.length > 0} />
          ) : (
            <CallTracePanel mocks={mocks} />
          )}
        </div>
      ) : null}
    </section>
  );
}
