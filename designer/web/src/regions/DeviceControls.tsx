// SPDX-License-Identifier: MIT

// The device's hardware strip and the app-state overlay (issue #448; `studio-device-profiles-v1.md` section 4).
//
// **The strip is generated from the profile's `controls` and from nothing else.** A desktop profile lists
// none, so it renders nothing; an Android phone lists six, so it renders six; a profile that drops Recents
// shows no Recents. Each button sends one normalized event (`designer-protocol-v1.md` section 5.1) through
// the page's platform hook, and the engine's reply -- not this component -- decides what the app does.
//
// **The overlay is the engine's reply made visible**: an unhandled Back says "App closed" with Relaunch,
// Home says "In background" with Resume, Power says "Screen off" with Wake (the studio plan's mock). It is
// page chrome laid over the canvas, so it never costs an engine frame, and it covers the canvas so a pointer
// cannot reach an app that is not in the foreground.
//
// Keyboard: the strip is a `role="toolbar"` and one tab stop with a roving tabindex; the arrow keys, Home
// and End move inside it, and every button has an accessible name.

import { useRef } from "react";
import type { DeviceProfile } from "../lib/device-profile";
import { CONTROL_CATALOG, STATE_PRESENTATION, type AppLifeState, type PlatformEventType } from "../lib/platform-events";

const GROUP_LABELS = { navigation: "Navigation", keys: "Side keys" } as const;

/** The strip of hardware or system buttons the profile offers; nothing at all when it offers none. */
export function HardwareStrip({
  profile,
  disabled,
  onPress,
}: {
  profile: DeviceProfile | null;
  /** True while a replay runs: a button pressed then would interleave with the replayed history. */
  disabled: boolean;
  onPress: (type: PlatformEventType) => void;
}) {
  const strip = useRef<HTMLDivElement>(null);
  if (profile === null || profile.controls.length === 0) {
    return null;
  }
  const groups = (["navigation", "keys"] as const)
    .map((group) => ({
      group,
      controls: profile.controls.filter((control) => CONTROL_CATALOG[control].group === group),
    }))
    .filter((entry) => entry.controls.length > 0);

  function move(event: React.KeyboardEvent<HTMLDivElement>) {
    const buttons = Array.from(strip.current?.querySelectorAll<HTMLButtonElement>("button") ?? []);
    const index = buttons.indexOf(document.activeElement as HTMLButtonElement);
    if (index < 0) {
      return;
    }
    let next = -1;
    if (event.key === "ArrowRight" || event.key === "ArrowDown") {
      next = (index + 1) % buttons.length;
    } else if (event.key === "ArrowLeft" || event.key === "ArrowUp") {
      next = (index - 1 + buttons.length) % buttons.length;
    } else if (event.key === "Home") {
      next = 0;
    } else if (event.key === "End") {
      next = buttons.length - 1;
    }
    if (next >= 0) {
      event.preventDefault();
      buttons.forEach((button, position) => {
        button.tabIndex = position === next ? 0 : -1;
      });
      buttons[next].focus();
    }
  }

  return (
    <div
      className="hw-strip"
      id="hardware-strip"
      role="toolbar"
      aria-label={`${profile.name} controls`}
      aria-orientation="horizontal"
      ref={strip}
      onKeyDown={move}
    >
      {groups.map(({ group, controls }, groupIndex) => (
        <div className="hw-group" key={group} role="group" aria-label={GROUP_LABELS[group]}>
          <span className="hw-caption" aria-hidden="true">
            {group === "navigation" ? `Navigation (${profile.navigation})` : GROUP_LABELS[group]}
          </span>
          {controls.map((control, index) => (
            <button
              key={control}
              type="button"
              id={`hw-${control}`}
              className="hw-button"
              aria-label={CONTROL_CATALOG[control].label}
              title={CONTROL_CATALOG[control].title}
              disabled={disabled}
              // Roving tabindex: one tab stop for the whole strip, on its first button.
              tabIndex={groupIndex === 0 && index === 0 ? 0 : -1}
              onClick={() => onPress(control)}
            >
              {CONTROL_CATALOG[control].label}
            </button>
          ))}
        </div>
      ))}
    </div>
  );
}

/**
 * The overlay over the canvas while the app is not in the foreground. Nothing while it is running, and
 * nothing for `inactive` (focus lost, still painted and running).
 */
export function AppStateOverlay({
  state,
  disabled,
  onAction,
}: {
  state: AppLifeState;
  disabled: boolean;
  onAction: (type: PlatformEventType) => void;
}) {
  const presentation = STATE_PRESENTATION[state];
  if (state === "running" || state === "inactive" || presentation.action === null) {
    return null;
  }
  const action = presentation.action;
  return (
    <div className="app-overlay" id="app-overlay" role="status" aria-live="polite" data-state={state}>
      <p>{presentation.message}</p>
      <button type="button" id="app-overlay-action" disabled={disabled} onClick={() => onAction(action.type)}>
        {action.label}
      </button>
    </div>
  );
}
