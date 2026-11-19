// SPDX-License-Identifier: MIT

// Platform events in the studio page (issues #446 and #448; `designer-protocol-v1.md` section 5.1 is
// normative for the wire names and the reply, `studio-device-profiles-v1.md` section 4 for the controls).
//
// Everything here is **pure**: the reply type, the catalog the panel and the strip are generated from, the
// ordered event log, and the record-and-replay script. No DOM, no React and no Wasm, so the Node gate
// (`scripts/ci/preview_platform_events.mjs`) imports the very code the page ships.
//
// **The script is the native scripted run's own format** (`tools/codegen/scene_interaction.cpp`): one step
// per line, `platform <type> [value] [@<timestampNs>]`. A log exported from the panel therefore runs under
// `tessera_scene_preview --input-json --script` unchanged, which is how a bug found by hand becomes a
// fixture the native tests and the parity gate can run (`os-events-v1.md` section 12).

import type { ControlId, EventId } from "./device-profile";

/** Every wire name the engine accepts (`PlatformEventKind` in `tools/codegen/scene_platform.hpp`). */
export type PlatformEventType =
  | ControlId
  | EventId
  | "activate"
  | "relaunch";

/** The engine's account of what the app is doing (`AppLife`). */
export type AppLifeState = "running" | "inactive" | "background" | "screenOff" | "closed";

/** The reply to one `platform.event` (designer-protocol-v1 section 5.1). */
export interface PlatformReply {
  ok: boolean;
  /** A `TESSERA_PLATFORM_*` code and a sentence, present when `ok` is false. */
  error?: string;
  type: PlatformEventType;
  handled: boolean;
  appClosed: boolean;
  resumed: boolean;
  state: AppLifeState;
  /** How a Back resolved: `unhandled`, or `none` for every other event. */
  dispatch: string;
  /** The engine's count of accepted events. */
  sequence: number;
  /** The simulated clock the event was stamped with, in nanoseconds. */
  timestampNs: number;
  /** The event moved a layout input (orientation, fold): a frame is owed. */
  changed: boolean;
  /** A Suspend captured the state cells. */
  savedState: boolean;
  /** A relaunch ended the evaluation: the page applies again, which is the cold start. */
  relaunch: boolean;
  /** The relaunch carries the saved cells into the new evaluation. */
  restore: boolean;
}

/** One event the page sends: the type, its single argument and the simulated timestamp. */
export interface PlatformEventRequest {
  type: PlatformEventType;
  value: string;
  timestampNs: number;
}

/** The control strip's buttons: label, group and accessible name (the strip is generated from the profile). */
export const CONTROL_CATALOG: Readonly<
  Record<ControlId, { label: string; group: "navigation" | "keys"; title: string }>
> = Object.freeze({
  back: { label: "Back", group: "navigation", title: "Back (BackRequested): overlay, then route, else the app closes" },
  home: { label: "Home", group: "navigation", title: "Home (WindowHidden, Suspend): the app goes to the background" },
  recents: { label: "Recents", group: "navigation", title: "Recents (WindowDeactivated): focus is lost, the app keeps running" },
  volumeUp: { label: "Volume up", group: "keys", title: "Volume up key, delivered to the focused node" },
  volumeDown: { label: "Volume down", group: "keys", title: "Volume down key, delivered to the focused node" },
  power: { label: "Power", group: "keys", title: "Power (ScreenOff, then ScreenOn on wake)" },
});

/** One button of the panel: what it is called and what it sends. */
export interface EventButton {
  label: string;
  type: PlatformEventType;
  value: string;
}

/**
 * The panel's buttons for each event a profile may list, in the order the panel shows them. An event with
 * values offers one button per value, so the argument is visible on the button (as in the studio plan's
 * `ThemeChanged: dark`) rather than hidden behind a menu.
 */
export const EVENT_CATALOG: Readonly<Record<EventId, readonly EventButton[]>> = Object.freeze({
  suspend: [{ label: "Suspend", type: "suspend", value: "" }],
  resume: [{ label: "Resume", type: "resume", value: "" }],
  screenOff: [{ label: "ScreenOff", type: "screenOff", value: "" }],
  screenOn: [{ label: "ScreenOn", type: "screenOn", value: "" }],
  theme: [
    { label: "ThemeChanged: dark", type: "theme", value: "dark" },
    { label: "ThemeChanged: light", type: "theme", value: "light" },
  ],
  locale: [
    { label: "LocaleChanged: vi-VN", type: "locale", value: "vi-VN" },
    { label: "LocaleChanged: en-US", type: "locale", value: "en-US" },
  ],
  textScale: [
    { label: "TextScaleChanged: 1.3", type: "textScale", value: "1.3" },
    { label: "TextScaleChanged: 1", type: "textScale", value: "1" },
  ],
  orientation: [
    { label: "OrientationChanged: portrait", type: "orientation", value: "portrait" },
    { label: "OrientationChanged: landscape", type: "orientation", value: "landscape" },
  ],
  fold: [
    { label: "FoldChanged: book", type: "fold", value: "book" },
    { label: "FoldChanged: tabletop", type: "fold", value: "tabletop" },
    { label: "FoldChanged: flat", type: "fold", value: "flat" },
    { label: "FoldChanged: none", type: "fold", value: "none" },
  ],
  ime: [
    { label: "ImeChanged: show", type: "ime", value: "show" },
    { label: "ImeChanged: hide", type: "ime", value: "hide" },
  ],
  memoryPressure: [{ label: "MemoryPressure", type: "memoryPressure", value: "" }],
  batterySaver: [
    { label: "BatterySaver: on", type: "batterySaver", value: "on" },
    { label: "BatterySaver: off", type: "batterySaver", value: "off" },
  ],
  thermal: [{ label: "Thermal: serious", type: "thermal", value: "serious" }],
  processDeath: [{ label: "Kill process, relaunch with saved state", type: "processDeath", value: "" }],
});

/** The sentence under an app state, and the action that leaves it (the studio plan's overlay). */
export const STATE_PRESENTATION: Readonly<
  Record<AppLifeState, { message: string; action: { label: string; type: PlatformEventType } | null }>
> = Object.freeze({
  running: { message: "Running", action: null },
  inactive: { message: "Inactive (focus lost)", action: { label: "Activate", type: "activate" } },
  background: { message: "In background", action: { label: "Resume", type: "resume" } },
  screenOff: { message: "Screen off", action: { label: "Wake", type: "power" } },
  closed: { message: "App closed", action: { label: "Relaunch", type: "relaunch" } },
});

/** One line of the ordered event log. */
export interface LogEntry {
  /** The engine's sequence number for the event. */
  sequence: number;
  /** The simulated timestamp, in nanoseconds. */
  timestampNs: number;
  type: PlatformEventType;
  value: string;
  /** What the event did, in words: the log's third column. */
  effect: string;
  ok: boolean;
  /** The app state after the event. */
  state: AppLifeState;
}

/** What a reply did, as one phrase for the log. */
export function effectOf(reply: PlatformReply): string {
  if (!reply.ok) {
    return reply.error ?? "refused";
  }
  if (reply.relaunch) {
    return reply.restore ? "relaunched, saved state restored" : "relaunched cold";
  }
  if (reply.type === "back") {
    return reply.appClosed ? "unhandled: app closed" : reply.handled ? "handled" : "not delivered";
  }
  if (reply.appClosed) {
    return "app closed";
  }
  if (reply.resumed) {
    return reply.state === "running" ? "resumed (state kept)" : `resumed to ${reply.state}`;
  }
  if (reply.type === "home" && reply.handled) {
    return reply.savedState ? "in background (state saved)" : "in background";
  }
  if (reply.type === "power" && reply.handled) {
    return "screen off";
  }
  if (reply.changed) {
    return "layout updated in place";
  }
  return reply.handled ? "handled" : "delivered, no effect";
}

/** The log entry for a reply. */
export function entryOf(request: PlatformEventRequest, reply: PlatformReply): LogEntry {
  return {
    sequence: reply.sequence,
    timestampNs: reply.ok ? reply.timestampNs : request.timestampNs,
    type: request.type,
    value: request.value,
    effect: effectOf(reply),
    ok: reply.ok,
    state: reply.state,
  };
}

/** The log's time column: milliseconds with one decimal, e.g. `1250.0 ms`. */
export function formatTimestamp(timestampNs: number): string {
  return `${(timestampNs / 1_000_000).toFixed(1)} ms`;
}

/** The most entries the log keeps; the engine's scripted run caps its own scripts the same way. */
export const MAX_LOG_ENTRIES = 256;

/** Appends one entry, dropping the oldest past the cap so a long session cannot grow without bound. */
export function appendEntry(log: readonly LogEntry[], entry: LogEntry): LogEntry[] {
  const next = [...log, entry];
  return next.length > MAX_LOG_ENTRIES ? next.slice(next.length - MAX_LOG_ENTRIES) : next;
}

const KNOWN_TYPES: ReadonlySet<string> = new Set([
  "back", "home", "recents", "volumeUp", "volumeDown", "power",
  "suspend", "resume", "activate", "screenOff", "screenOn",
  "theme", "locale", "textScale", "orientation", "fold", "ime",
  "memoryPressure", "batterySaver", "thermal", "processDeath", "relaunch",
]);

/** Whether `name` is a wire name of section 5.1. */
export function isPlatformEventType(name: string): name is PlatformEventType {
  return KNOWN_TYPES.has(name);
}

/**
 * The record-and-replay script for a log: the accepted events, in order, with their simulated timestamps.
 *
 * A refused event (a `relaunch` of a running app, an unknown value) is left out: the script is a fixture
 * that must run to the same final state, and a step the engine refuses is not part of how it got there.
 */
export function exportScript(log: readonly LogEntry[]): string {
  const lines = [
    "# Tessera platform events (record and replay script).",
    "# One step per line: platform <type> [value] [@<timestampNs>].",
    "# Replay in the studio, or natively: tessera_scene_preview --input-json OUT --script THIS FILE.",
  ];
  for (const entry of log) {
    if (!entry.ok) {
      continue;
    }
    const value = entry.value === "" ? "" : ` ${entry.value}`;
    lines.push(`platform ${entry.type}${value} @${Math.round(entry.timestampNs)}`);
  }
  return lines.join("\n") + "\n";
}

/** A script that does not parse: the line number and a sentence, never a silently dropped step. */
export class ScriptError extends Error {
  readonly line: number;
  constructor(line: number, message: string) {
    super(`line ${line}: ${message}`);
    this.name = "ScriptError";
    this.line = line;
  }
}

/** The most steps a script may hold (the engine's own cap on a scripted run). */
export const MAX_SCRIPT_STEPS = 64;

/**
 * Parses a replay script into the events to send. Blank lines and `#` comments are skipped; a `click` step
 * (the native script also carries clicks) is refused, because the studio replays platform events only.
 */
export function parseScript(text: string): PlatformEventRequest[] {
  const steps: PlatformEventRequest[] = [];
  const lines = text.split(/\r?\n/);
  for (let index = 0; index < lines.length; index += 1) {
    const trimmed = lines[index].trim();
    if (trimmed === "" || trimmed.startsWith("#")) {
      continue;
    }
    const words = trimmed.split(/\s+/);
    if (words[0] !== "platform") {
      throw new ScriptError(index + 1, "expected `platform <type> [value] [@ns]`");
    }
    const type = words[1];
    if (type === undefined || !isPlatformEventType(type)) {
      throw new ScriptError(index + 1, `\`${type ?? ""}\` is not a platform event type`);
    }
    let value = "";
    let timestampNs = -1;
    for (const word of words.slice(2)) {
      if (word.startsWith("@")) {
        const parsed = Number(word.slice(1));
        if (!/^@\d+$/.test(word) || !Number.isSafeInteger(parsed)) {
          throw new ScriptError(index + 1, `the timestamp must be \`@<nanoseconds>\`, got \`${word}\``);
        }
        timestampNs = parsed;
      } else if (value === "") {
        value = word;
      } else {
        throw new ScriptError(index + 1, `\`platform\` takes one value, got \`${word}\` as well`);
      }
    }
    if (steps.length >= MAX_SCRIPT_STEPS) {
      throw new ScriptError(index + 1, `the script has more than ${MAX_SCRIPT_STEPS} steps`);
    }
    steps.push({ type, value, timestampNs });
  }
  return steps;
}

/**
 * A reply for a call that never reached the engine (no module, no session, an old build) or that it
 * answered with something that is not a reply. It is an `ok:false` reply so the log and the panel have one
 * shape to show; the `TESSERA_PLATFORM_UNAVAILABLE` code keeps it apart from the engine's own refusals.
 */
export function unavailableReply(type: PlatformEventType, reason: string): PlatformReply {
  return {
    ok: false,
    error: `TESSERA_PLATFORM_UNAVAILABLE: ${reason}`,
    type,
    handled: false,
    appClosed: false,
    resumed: false,
    state: "running",
    dispatch: "none",
    sequence: 0,
    timestampNs: 0,
    changed: false,
    savedState: false,
    relaunch: false,
    restore: false,
  };
}

/** Parses the engine's reply document into a reply; a document that is not one becomes an unavailable reply. */
export function parseReply(type: PlatformEventType, text: string): PlatformReply {
  try {
    const value: unknown = JSON.parse(text);
    if (typeof value !== "object" || value === null || typeof (value as PlatformReply).ok !== "boolean") {
      return unavailableReply(type, "the engine's platform reply is not a reply object");
    }
    return { ...unavailableReply(type, ""), ...(value as PlatformReply) };
  } catch (error) {
    return unavailableReply(type, `the engine's platform reply is not JSON: ${String(error)}`);
  }
}
