// SPDX-License-Identifier: MIT

// The canvas pane's animation frame loop (issue #603), kept free of the DOM and the module so the gate
// can run it in plain Node.
//
// **What the page owes the engine.** The preview is render-on-demand: a static scene draws once and then
// nothing (`canvas-session.ts`, rule 1). An animation is the one case where the engine needs frames
// without any input, and the engine says so: `tessera_canvas_draw`'s report carries `animating`, true
// while the animation runtime holds an entry. This loop is the page's half of that contract:
//
//   - while `animating` is true, ask for the next frame, hand the engine its timestamp (`tessera_canvas_tick`)
//     and draw;
//   - the moment it is false, stop. No frame is requested, so an idle canvas costs nothing;
//   - never keep two chains alive: an input-driven frame and an animation frame that land in the same
//     display refresh are one frame, not two.
//
// The timestamp is the one the browser gives `requestAnimationFrame`, in milliseconds, so the engine's
// clock is the display's clock and a hidden tab (no callbacks) simply does not advance time.

/** What the engine said about the frame it just drew. */
export interface MotionFrameResult {
  /** An animation is running: another frame is owed. */
  animating: boolean;
}

/** The three things the loop needs from its host. */
export interface MotionHost {
  /** Schedules `callback` for the next display refresh (`requestAnimationFrame`). */
  request(callback: (nowMs: number) => void): void;
  /**
   * Advances the engine's animation clock to `nowMs`, draws, and answers whether an animation is still
   * running. A throw or a false answer ends the chain.
   */
  frame(nowMs: number): MotionFrameResult | null;
  /** Another frame (an input, an edit, a resize) is already queued, so the chain can yield to it. */
  pending(): boolean;
}

/** A running chain of animation frames. */
export interface MotionLoop {
  /** The engine reported `animating` after a draw: keep the chain alive. Idempotent. */
  keepAlive(animating: boolean): void;
  /** Whether a frame is scheduled by this loop. */
  scheduled(): boolean;
  /** Frames this loop drew (not counting input-driven ones), for the gate. */
  drawn(): number;
}

export function createMotionLoop(host: MotionHost): MotionLoop {
  let scheduledFrame = false;
  let drawnFrames = 0;

  function step(nowMs: number): void {
    scheduledFrame = false;
    // An input-driven frame is queued: it draws (and ticks) this refresh and re-arms the chain from its
    // own report, so drawing here too would present twice in one refresh.
    if (host.pending()) {
      return;
    }
    let result: MotionFrameResult | null = null;
    try {
      result = host.frame(nowMs);
    } catch {
      result = null;
    }
    drawnFrames += 1;
    keepAlive(result !== null && result.animating);
  }

  function keepAlive(animating: boolean): void {
    if (!animating || scheduledFrame) {
      return;
    }
    scheduledFrame = true;
    host.request(step);
  }

  return {
    keepAlive,
    scheduled: () => scheduledFrame,
    drawn: () => drawnFrames,
  };
}

/** `prefers-reduced-motion: reduce`, read when a frame is ticked so a system change applies at once. */
export function prefersReducedMotion(
  matchMedia: ((query: string) => { matches: boolean }) | undefined,
): boolean {
  if (matchMedia === undefined) {
    return false;
  }
  return matchMedia("(prefers-reduced-motion: reduce)").matches;
}

/** The engine's tick report (`tessera_canvas_tick`), parsed and defaulted. */
export interface TickReport {
  running: boolean;
  dtMs: number;
  samples: number;
  reducedMotion: boolean;
}

export function parseTickReport(text: string): TickReport {
  const raw = JSON.parse(text) as Partial<TickReport> & { tickVersion?: number };
  return {
    running: raw.running === true,
    dtMs: typeof raw.dtMs === "number" ? raw.dtMs : 0,
    samples: typeof raw.samples === "number" ? raw.samples : 0,
    reducedMotion: raw.reducedMotion === true,
  };
}
