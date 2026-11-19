// SPDX-License-Identifier: MIT

// The bottom dock's **Platform events** panel (issue #448; `studio-device-profiles-v1.md` section 4; the
// studio plan's dock). It offers the events the current profile lists, shows the app's state and the action
// that leaves it, and keeps the ordered event log with export and replay.
//
// **Everything it offers comes from the profile** (`profile.events`) and the catalog in
// `lib/platform-events.ts`; the engine answers every press and the log shows what it said. The panel is a
// `role="toolbar"` over its buttons, one tab stop with a roving tabindex, so a keyboard user does not tab
// through fourteen buttons to reach the log.
//
// **The log is the fixture.** Export writes the native scripted run's own format
// (`platform <type> [value] [@ns]`), so a bug found by hand replays here, or under
// `tessera_scene_preview --input-json --script`, to the same final state.

import { useRef } from "react";
import type { DeviceProfile } from "../lib/device-profile";
import {
  EVENT_CATALOG,
  STATE_PRESENTATION,
  formatTimestamp,
  type EventButton,
  type LogEntry,
  type PlatformEventType,
} from "../lib/platform-events";
import type { PlatformControls } from "../page-platform";

/** The panel's buttons for a profile, in the profile's order (the catalog's own order within an event). */
function buttonsOf(profile: DeviceProfile | null): EventButton[] {
  if (profile === null) {
    return [];
  }
  return profile.events.flatMap((event) => EVENT_CATALOG[event]);
}

function download(name: string, text: string): void {
  const url = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = name;
  document.body.appendChild(link);
  link.click();
  link.remove();
  // The click has started the download; the object URL is not needed after that.
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function LogRow({ entry, index }: { entry: LogEntry; index: number }) {
  return (
    <li className={"log-row" + (entry.ok ? "" : " is-refused")} data-sequence={entry.sequence} value={index + 1}>
      <span className="log-seq">#{entry.sequence}</span>
      <span className="log-time">{formatTimestamp(entry.timestampNs)}</span>
      <span className="log-type">
        {entry.type}
        {entry.value === "" ? "" : `: ${entry.value}`}
      </span>
      <span className="log-effect">{entry.effect}</span>
    </li>
  );
}

export function PlatformDock({
  profile,
  platform,
  onRestart,
}: {
  /** The profile in force (the custom size's own profile when none is picked). */
  profile: DeviceProfile | null;
  platform: PlatformControls;
  /** The **Restart app** action: a cold start, state reset. */
  onRestart: () => void;
}) {
  const bar = useRef<HTMLDivElement>(null);
  const file = useRef<HTMLInputElement>(null);
  const buttons = buttonsOf(profile);
  const presentation = STATE_PRESENTATION[platform.state];

  function move(event: React.KeyboardEvent<HTMLDivElement>) {
    const all = Array.from(bar.current?.querySelectorAll<HTMLButtonElement>("button") ?? []);
    const index = all.indexOf(document.activeElement as HTMLButtonElement);
    if (index < 0) {
      return;
    }
    let next = -1;
    if (event.key === "ArrowRight") {
      next = (index + 1) % all.length;
    } else if (event.key === "ArrowLeft") {
      next = (index - 1 + all.length) % all.length;
    } else if (event.key === "Home") {
      next = 0;
    } else if (event.key === "End") {
      next = all.length - 1;
    }
    if (next >= 0) {
      event.preventDefault();
      all.forEach((button, position) => {
        button.tabIndex = position === next ? 0 : -1;
      });
      all[next].focus();
    }
  }

  return (
    <section className="dock" id="platform-dock" aria-label="Bottom dock">
      <div className="dock-tabs" role="tablist" aria-label="Dock panels">
        {/* One panel today. The Mock console and Call trace tabs of the plan arrive with ADR-055. */}
        <button type="button" role="tab" id="dock-tab-platform" className="tab" aria-selected="true" aria-controls="dock-panel-platform">
          Platform events
        </button>
      </div>
      <div className="dock-panel" id="dock-panel-platform" role="tabpanel" aria-labelledby="dock-tab-platform">
        <div className="dock-state">
          <span className="dock-state-label">App state</span>
          <strong id="platform-state" data-state={platform.state} role="status" aria-live="polite">
            {presentation.message}
          </strong>
          {presentation.action !== null ? (
            <button
              type="button"
              id="platform-state-action"
              disabled={platform.replaying}
              onClick={() => void platform.send(presentation.action?.type as PlatformEventType)}
            >
              {presentation.action.label}
            </button>
          ) : null}
          <button type="button" id="platform-restart" disabled={platform.replaying} onClick={onRestart}>
            Restart app
          </button>
        </div>

        {buttons.length > 0 ? (
          <div className="evb" id="platform-events" role="toolbar" aria-label="Platform events" ref={bar} onKeyDown={move}>
            {buttons.map((button, index) => (
              <button
                key={`${button.type}:${button.value}`}
                type="button"
                data-event={button.type}
                data-value={button.value}
                disabled={platform.replaying}
                tabIndex={index === 0 ? 0 : -1}
                onClick={() => void platform.send(button.type, button.value)}
              >
                {button.label}
              </button>
            ))}
          </div>
        ) : (
          <p className="dock-empty">This device offers no platform events.</p>
        )}

        <div className="log-head">
          <span id="log-heading">Event log (simulated clock, in order)</span>
          <span className="log-actions">
            <button
              type="button"
              id="log-export"
              disabled={platform.log.length === 0}
              onClick={() => download("platform-events.txt", platform.exportText())}
            >
              Export
            </button>
            <button
              type="button"
              id="log-replay"
              disabled={platform.log.length === 0 || platform.replaying}
              onClick={() => void platform.replayLog()}
            >
              Replay
            </button>
            <button type="button" id="log-import" disabled={platform.replaying} onClick={() => file.current?.click()}>
              Import and replay
            </button>
            <input
              ref={file}
              id="log-import-file"
              type="file"
              accept=".txt,text/plain"
              hidden
              onChange={(event) => {
                const chosen = event.currentTarget.files?.[0];
                event.currentTarget.value = "";
                if (chosen !== undefined) {
                  void chosen.text().then((text) => platform.replay(text));
                }
              }}
            />
          </span>
        </div>
        <p className="log-notice" id="log-notice" role="status" hidden={platform.notice === null}>
          {platform.notice}
        </p>
        {platform.log.length === 0 ? (
          <p className="dock-empty" id="log-empty">
            No events yet. Press a hardware button or an event above.
          </p>
        ) : (
          <ol className="log" id="platform-log" aria-labelledby="log-heading">
            {platform.log.map((entry, index) => (
              <LogRow key={`${entry.sequence}:${index}`} entry={entry} index={index} />
            ))}
          </ol>
        )}
      </div>
    </section>
  );
}
