// SPDX-License-Identifier: MIT

// The live edit's trigger (issue #444): the one place that decides *when* an edit is applied.
//
// **Why this is a module and not a line in a handler.** The page has three ways an edit can arrive (a
// keystroke in the editor, a buffer the page wrote itself, an explicit `Ctrl+Enter`) and three panes that
// must all show the same source afterwards. If each of those called its own `apply()` there would be
// three debounce timers racing, and the canvas would show a different revision from the diagnostics list.
// So the trigger is one controller with one timer, and every path goes through it:
//
//   * `schedule()` -- a keystroke. Starts (or restarts) the 150 ms timer. A burst of keystrokes is one
//     timer restart and therefore one apply.
//   * `applyNow()` -- `Ctrl+Enter`, and the first paint. Cancels a pending timer and applies immediately.
//   * `restart()` -- the **Restart app** button. A cold start of every mounted session, state reset.
//
// **What "one frame" means here.** This controller does not draw anything and knows nothing about a
// canvas. It calls the *session's* `apply()`, and the Canvas session is what coalesces a burst into one
// frame (its `requestFrame()` returns while a frame is already pending). So the two coalescers compose
// exactly once each: this timer collapses a burst of keystrokes into one apply, and `requestFrame()`
// collapses a burst of applies into one frame -- without either of them re-arming itself, which is the
// repository's idle rule (`docs/design/dirty-rendering.md`) applied to the page.
//
// **What this controller deliberately does not do.** It never inspects the source. Deciding whether an
// edit keeps or drops a state cell belongs to the engine (`scene_state.hpp`'s carry rule, which the
// canvas session reports as `droppedComponent`), and deciding whether a file is a source file belongs to
// the workspace (`project-paths.js`). A second copy of either rule here would be a second answer that
// could disagree with the picture.

import type { ViewDefinition, ViewSession } from "./lib/views";

/**
 * The debounce the spec names (`studio-device-profiles-v1.md` section 6: "about 150 ms debounce").
 *
 * Short enough that a paused typist sees the result before they look up, long enough that ordinary
 * typing between two words is one apply. It is a delay, not a rate limit: the timer restarts on every
 * keystroke, so a continuous burst applies once, 150 ms after the last one.
 */
export const APPLY_DEBOUNCE_MS = 150;

/** What the controller needs from the page: the sessions, and which one is on screen. */
export interface LiveEditOptions {
  /** The mounted sessions, keyed by view. Read fresh on every call; the object is a stable ref. */
  sessions: () => Partial<Record<ViewDefinition["id"], ViewSession>>;
  /**
   * The view the preview column is showing.
   *
   * Not which pane an edit applies to -- an edit applies to **all three** (`studio-page.md`: "applies to
   * all three panes from one source of edits"), because a developer who edits on the Canvas tab must
   * find the Diagnostics pane already describing that text when they switch to it. This is only used to
   * order the work (the visible pane first, so what the developer is looking at updates first) and to
   * attribute a failure in the console.
   */
  currentView: () => ViewDefinition["id"];
  /**
   * The project file the keystroke landed in, read at the moment of the keystroke.
   *
   * It is the *edited* file, not the compile target: a keystroke in `theme.css` edits a project file
   * that is not a source file, and the spec's reset case is about that file, not about which source
   * happens to be the compile target at the time. Only the path is read, never the text: comparing
   * paths is enough to classify the edit, and reading text here would make the controller a second
   * reader of the source.
   */
  editedFile: () => string;
  /** The workspace's `FILE_KIND` for a project path, so "is this a source file?" is not re-derived. */
  kindOf: (path: string) => string;
  /** A timer was scheduled, applied, or cancelled: for the page's own "applying..." hint. Optional. */
  onStateChange?: (state: LiveEditState) => void;
}

/** What the trigger is doing, so the page can say so without reading the timer. */
export interface LiveEditState {
  /** An apply is scheduled or running: the preview is behind the editor. */
  applying: boolean;
  /**
   * Why the last edit restarted the app rather than applying, when it did.
   *
   * `null` for an ordinary live apply. The page shows this where it already explains what **Restart
   * app** does, because a restart a developer did not ask for must never look like a bug.
   */
  restartedBecause: "non-source-file" | null;
}

export interface LiveEdit {
  /** A keystroke landed. Debounced: one apply per burst. */
  schedule(): void;
  /** `Ctrl+Enter`: apply immediately, cancelling any debounce. Also the first-load auto-start. */
  applyNow(): Promise<void>;
  /** **Restart app**: a cold start of every mounted session, state reset. */
  restart(): Promise<void>;
  /** Drops a pending apply (a restart supersedes it, and so does leaving the page). */
  cancel(): void;
  /** Whether an apply is scheduled or in flight. */
  pending(): boolean;
}

/**
 * Creates the trigger. One per page, created once: it owns the only debounce timer on the page.
 *
 * `timers` is injectable so the smoke test can drive the debounce without waiting on wall-clock time;
 * every argument it needs is already a function, so nothing here reaches into the DOM.
 */
export function createLiveEdit(
  options: LiveEditOptions,
  timers: { setTimeout: (fn: () => void, ms: number) => number; clearTimeout: (id: number) => void } = {
    setTimeout: (fn, ms) => globalThis.setTimeout(fn, ms),
    clearTimeout: (id) => globalThis.clearTimeout(id),
  },
): LiveEdit {
  let timer: number | null = null;
  let applying = false;
  let restartedBecause: LiveEditState["restartedBecause"] = null;
  // Serializes applies: a second one may not start while a previous one is still running (a Wasm
  // evaluation is async through its module import), and the newest request wins. This is a
  // "coalesce to the latest" queue of depth one, not a frame loop.
  let inFlight: Promise<void> | null = null;
  let queued: Promise<void> | null = null;

  function report(next: { applying: boolean; restartedBecause?: LiveEditState["restartedBecause"] }): void {
    applying = next.applying;
    if (next.restartedBecause !== undefined) {
      restartedBecause = next.restartedBecause;
    }
    options.onStateChange?.({ applying, restartedBecause });
  }

  /**
   * The mounted sessions, the visible one first.
   *
   * Order matters for latency, not for correctness: the pane the developer is looking at must update
   * before the two they are not, and awaiting them in order is the cheapest way to say that without
   * running three Wasm evaluations in parallel (which the modules are not built for).
   */
  function ordered(): Array<{ id: ViewDefinition["id"]; session: ViewSession }> {
    const mounted = options.sessions();
    const current = options.currentView();
    const out: Array<{ id: ViewDefinition["id"]; session: ViewSession }> = [];
    const entries = Object.entries(mounted) as Array<[ViewDefinition["id"], ViewSession | undefined]>;
    for (const [id, candidate] of entries) {
      if (candidate !== undefined && id === current) {
        out.push({ id, session: candidate });
      }
    }
    for (const [id, candidate] of entries) {
      if (candidate !== undefined && id !== current) {
        out.push({ id, session: candidate });
      }
    }
    return out;
  }

  /** Applies the source to every mounted session, coalescing a call made during an apply. */
  function applyOnce(): Promise<void> {
    if (inFlight !== null) {
      // A request arrived while one is running: remember only the newest, so a burst of applies during
      // a slow evaluation is one extra apply rather than a queue that grows with the typing.
      if (queued === null) {
        queued = inFlight.then(() => {
          queued = null;
          return applyOnce();
        });
      }
      return queued;
    }
    const targets = ordered().filter(({ session: candidate }) => typeof candidate.apply === "function");
    if (targets.length === 0) {
      return Promise.resolve();
    }
    report({ applying: true, restartedBecause: null });
    inFlight = (async () => {
      for (const { id, session: candidate } of targets) {
        try {
          await candidate.apply?.();
        } catch (error) {
          // A session's `apply()` reports its own failures in its own pane; reaching here means the
          // session threw outside that contract. The console keeps the detail, and one pane's bug does
          // not stop the other two from showing the same source.
          console.error(`the ${id} view failed to apply the edit:`, error);
        }
      }
    })().finally(() => {
      inFlight = null;
      report({ applying: false });
    });
    return inFlight;
  }

  function cancel(): void {
    if (timer !== null) {
      timers.clearTimeout(timer);
      timer = null;
      report({ applying: false });
    }
  }

  function schedule(): void {
    // The spec's reset case, checked before the debounce because a restart is not an edit: a project
    // file that is not a source file (the config, a theme) is not compiled by a live apply, so the
    // honest answer is the cold start a launch performs. A stylesheet is the clearest example -- a
    // keystroke there changes nothing a preview can show yet (#390/#391), and silently applying the
    // *source* instead would look like the edit had been ignored.
    const file = options.editedFile();
    if (file !== "" && options.kindOf(file) !== "source") {
      void restart("non-source-file");
      return;
    }
    if (timer !== null) {
      timers.clearTimeout(timer);
    }
    report({ applying: true });
    timer = timers.setTimeout(() => {
      timer = null;
      void applyOnce();
    }, APPLY_DEBOUNCE_MS);
  }

  async function applyNow(): Promise<void> {
    cancel();
    await applyOnce();
  }

  async function restart(because: LiveEditState["restartedBecause"] = null): Promise<void> {
    cancel();
    report({ applying: true, restartedBecause: because });
    // Every mounted session restarts, not only the visible one: the hidden panes would otherwise show
    // state from before the cold start the moment a developer switched to them, which is exactly the
    // "one source of edits, three panes" rule the issue asks for.
    for (const { id, session: candidate } of ordered()) {
      try {
        await candidate.run();
      } catch (error) {
        console.error(`the ${id} view failed to restart:`, error);
      }
    }
    report({ applying: false });
  }

  return {
    schedule,
    applyNow,
    restart: () => restart(null),
    cancel,
    pending: () => timer !== null || inFlight !== null,
  };
}
