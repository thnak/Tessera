// SPDX-License-Identifier: MIT

// The page's platform-event state (issue #448; `studio-device-profiles-v1.md` section 4).
//
// **What this hook owns.** The emulated app's life state as the engine last reported it, the ordered event
// log, and the simulated clock the events are stamped with. It does not decide what an event means: it sends
// one normalized event through the canvas session (`platform.event`, `designer-protocol-v1.md` section 5.1)
// and records the reply, so the panel, the control strip and the overlay all read what the engine said.
//
// **Determinism.** The timestamp on an event is the *simulated* clock: milliseconds since the app's last cold
// start, rounded to whole milliseconds. A replay sends the recorded timestamps instead of reading the clock,
// so the engine sees the same stream and reaches the same state (the log is the fixture).
//
// **A cold start ends the log.** The log is "what happened since the app launched", which is the only span a
// replay can reproduce from a launch; `reset()` clears it and restarts the clock. A `relaunch` is *not* a cold
// start of the page's own: the engine ends the evaluation and the log goes on, because the relaunch is one of
// the steps a replay performs.

import { useCallback, useRef, useState } from "react";
import {
  appendEntry,
  entryOf,
  exportScript,
  parseScript,
  unavailableReply,
  type AppLifeState,
  type LogEntry,
  type PlatformEventRequest,
  type PlatformEventType,
  type PlatformReply,
} from "./lib/platform-events";

/** What the page hands the hook: how to reach the canvas session and what to do with a layout change. */
export interface PlatformDeps {
  /** Delivers one event to the running canvas session. */
  deliver: (request: PlatformEventRequest) => PlatformReply;
  /** Starts the app from a launch (the **Restart app** action); the page resets the platform state too. */
  coldStart: () => Promise<void> | void;
  /** An orientation or fold event moved a layout input the toolbar also holds. */
  onLayout: (request: PlatformEventRequest, reply: PlatformReply) => void;
}

export interface PlatformState {
  /** What the app is doing, from the engine's last reply. */
  state: AppLifeState;
  /** The ordered event log since the last cold start. */
  log: readonly LogEntry[];
  /** A sentence about the last replay or import, or `null`. */
  notice: string | null;
  /** True while a replay is running, so the panel can say so and refuse a second one. */
  replaying: boolean;
}

export interface PlatformControls extends PlatformState {
  /** Sends one event and records it. */
  send: (type: PlatformEventType, value?: string) => Promise<PlatformReply>;
  /** The page started the app from a launch: the log and the clock start over. */
  reset: () => void;
  /** The record-and-replay script for the log. */
  exportText: () => string;
  /** Restarts the app and replays `text` against it. */
  replay: (text: string) => Promise<void>;
  /** Replays the page's own log. */
  replayLog: () => Promise<void>;
}

const NEXT_FRAME = (): Promise<void> => new Promise((resolve) => requestAnimationFrame(() => resolve()));

/** How many animation frames a replay waits for the restarted app to exist before it gives up. */
const START_WAIT_FRAMES = 180;

export function usePlatform(deps: PlatformDeps): PlatformControls {
  const [state, setState] = useState<AppLifeState>("running");
  const [log, setLog] = useState<readonly LogEntry[]>([]);
  const [notice, setNotice] = useState<string | null>(null);
  const [replaying, setReplaying] = useState(false);
  // The origin of the simulated clock: `performance.now()` at the app's last cold start.
  const origin = useRef(typeof performance === "undefined" ? 0 : performance.now());
  // The deps are read through a ref so the callbacks below are stable and a render never re-creates them.
  const depsRef = useRef(deps);
  depsRef.current = deps;
  const logRef = useRef<readonly LogEntry[]>(log);
  logRef.current = log;

  const reset = useCallback(() => {
    origin.current = performance.now();
    logRef.current = [];
    setLog([]);
    setState("running");
    setNotice(null);
  }, []);

  const deliver = useCallback((request: PlatformEventRequest): PlatformReply => {
    const reply = depsRef.current.deliver(request);
    const entry = entryOf(request, reply);
    logRef.current = appendEntry(logRef.current, entry);
    setLog(logRef.current);
    if (reply.ok) {
      setState(reply.state);
      if (reply.changed) {
        depsRef.current.onLayout(request, reply);
      }
    }
    return reply;
  }, []);

  const send = useCallback(
    async (type: PlatformEventType, value = ""): Promise<PlatformReply> => {
      // Whole simulated milliseconds, so the recorded script is short and exact.
      const timestampNs = Math.round(performance.now() - origin.current) * 1_000_000;
      return deliver({ type, value, timestampNs });
    },
    [deliver],
  );

  const replay = useCallback(
    async (text: string): Promise<void> => {
      let steps: PlatformEventRequest[];
      try {
        steps = parseScript(text);
      } catch (error) {
        setNotice(`replay refused: ${error instanceof Error ? error.message : String(error)}`);
        return;
      }
      if (steps.length === 0) {
        setNotice("replay refused: the script has no platform steps");
        return;
      }
      setReplaying(true);
      setNotice(null);
      try {
        await depsRef.current.coldStart();
        reset();
        // The restarted app exists after the canvas applies once; ask until the engine says it does.
        let waited = 0;
        let first = true;
        for (const step of steps) {
          let reply = unavailableReply(step.type, "the app did not restart");
          for (;;) {
            // The first step is asked repeatedly while the restarted app is still being applied; once it
            // is accepted the rest are delivered exactly once, in order.
            reply = deliver(step);
            if (!first || reply.ok || !reply.error?.includes("TESSERA_PLATFORM_NO_SESSION") || waited >= START_WAIT_FRAMES) {
              break;
            }
            // A refused probe is not part of the replayed history.
            logRef.current = logRef.current.slice(0, -1);
            setLog(logRef.current);
            waited += 1;
            await NEXT_FRAME();
          }
          first = false;
          if (!reply.ok) {
            setNotice(`replay stopped at ${step.type}: ${reply.error ?? "refused"}`);
            return;
          }
        }
        setNotice(`replayed ${steps.length} event${steps.length === 1 ? "" : "s"}`);
      } finally {
        setReplaying(false);
      }
    },
    [deliver, reset],
  );

  const exportText = useCallback(() => exportScript(logRef.current), []);
  const replayLog = useCallback(() => replay(exportScript(logRef.current)), [replay]);

  return { state, log, notice, replaying, send, reset, exportText, replay, replayLog };
}
