// SPDX-License-Identifier: MIT

// The canvas' drag-resize handles (issue #690 slice b): three grips on the right edge, the bottom edge
// and the corner, a live readout, and the coalescing that keeps a drag from becoming a hundred frames.
//
// **Why the DOM half is separate from the arithmetic.** `canvas-resize.ts` is pure and gated by
// `preview_canvas_resize.mjs`; this file owns the parts a Node gate cannot see - pointer capture,
// `requestAnimationFrame`, and the readout element. Keeping them apart means the rule that decides how
// often the engine hears about a drag is tested directly, rather than inferred from a browser.
//
// **Nothing here dispatches into the app.** A grip covers a strip of the canvas' edge, so it has to
// win the press before `attachCanvasPointer` sees it - the listener is in the capture phase and stops
// propagation, exactly like the style inspector's click. A press that reached the engine would move
// `:hover` and possibly activate the control under the grip.
//
// **A drag switches the device to `custom`.** The reached size is applied through the page's own
// control handler, so the profile picker shows `custom` afterwards and skins, zoom and the DPR stay
// consistent with the size the canvas is actually showing.

import { draggedViewport, resizeReadout, shouldStep } from "./canvas-resize";
import type { Viewport } from "./canvas-viewport";
import type { StatusReporter } from "../lib/views";

/** The grips the page renders, with their cursor and accessible name (read by the SSR gate). */
export const RESIZE_HANDLES = [
  { id: "canvas-resize-right", handle: "right", cursor: "ew-resize", label: "Resize width" },
  { id: "canvas-resize-bottom", handle: "bottom", cursor: "ns-resize", label: "Resize height" },
  { id: "canvas-resize-corner", handle: "corner", cursor: "nwse-resize", label: "Resize" },
] as const;

type DragHandle = (typeof RESIZE_HANDLES)[number]["handle"];

/** One display refresh, the fallback interval for the coalescing rule. */
const FRAME_MS = 16;

export interface CanvasResizeOptions {
  /**
   * The readout element a drag writes the pending size into, or `null` for none.
   *
   * Resolved by the caller rather than looked up here, so a pane rendered without one is a valid pane
   * and a test can pass a plain element.
   */
  readout: HTMLElement | null;
  /** CSS pixels on screen per CSS pixel of the canvas' layout box, i.e. the pane's zoom. */
  scale: () => number;
  /** The viewport in force, so a drag starts from what is on screen rather than from a stored value. */
  current: () => Viewport;
  /** Hands a reached size to the page's control handler. */
  apply: (width: number, height: number) => void;
  setStatus: StatusReporter;
}

export interface CanvasResize {
  dispose: () => void;
}

export function createCanvasResize(options: CanvasResizeOptions): CanvasResize {
  let drag: {
    pointerId: number;
    handle: DragHandle;
    /** The pointer's position at pointer-down, in client pixels. */
    origin: { x: number; y: number };
    /** The viewport in force at pointer-down. */
    from: Viewport;
    /** The travel (and time) of the last step that reached the engine. */
    previous: { size: { width: number; height: number } | null; atMs: number };
    /** The travel at the last pointer event, what a frame consumes. */
    travel: { x: number; y: number };
    /** The pending frame request, so a burst of moves asks for one. */
    frame: number | null;
  } | null = null;
  const cleanups: Array<() => void> = [];

  function now(): number {
    return globalThis.performance !== undefined && typeof globalThis.performance.now === "function"
      ? globalThis.performance.now()
      : Date.now();
  }

  function ratio(): number {
    const scale = options.scale();
    return Number.isFinite(scale) && scale > 0 ? scale : 1;
  }

  /**
   * Writes the readout, resolving it on first use.
   *
   * **Not resolved at construction.** The session is created from refs that exist at the first
   * settle, which is before React commits the pane's grips and readout, so a lookup in
   * `createCanvasResize` finds nothing and every write goes nowhere - the same bug the grips had
   * when their listeners were looked up by id.
   */
  let readoutElement: HTMLElement | null = options.readout;
  function setReadout(text: string): void {
    if (readoutElement === null) {
      readoutElement = document.getElementById("canvas-resize-readout");
    }
    if (readoutElement !== null) {
      readoutElement.textContent = text;
    }
  }

  /**
   * One step: apply the size at the current travel if the rule allows it.
   *
   * `settle` is pointer-up, which always applies so the size the person released on is the size the
   * engine ends with, even when the release fell between two animation frames. Applying an unchanged
   * size costs no frame: the engine answers `changed:false`.
   */
  function step(settle: boolean): void {
    if (drag === null) {
      return;
    }
    const viewport = draggedViewport(drag.from, drag.handle, drag.travel, ratio());
    setReadout(resizeReadout(viewport));
    if (viewport === null) {
      return;
    }
    const size = { width: viewport.width, height: viewport.height };
    const atMs = now();
    if (!shouldStep(drag.previous, { size, atMs }, settle, FRAME_MS)) {
      return;
    }
    drag.previous = { size, atMs };
    options.apply(size.width, size.height);
    if (viewport.clamped) {
      // A clamp is named rather than silent, the same rule a toolbar move follows.
      options.setStatus(`viewport clamped to ${resizeReadout(viewport)}`, "warning");
    }
  }

  function cancelFrame(): void {
    if (drag !== null && drag.frame !== null) {
      if (typeof globalThis.cancelAnimationFrame === "function") {
        globalThis.cancelAnimationFrame(drag.frame);
      }
      drag.frame = null;
    }
  }

  function schedule(): void {
    if (drag === null || drag.frame !== null) {
      return;
    }
    if (typeof globalThis.requestAnimationFrame !== "function") {
      step(false);
      return;
    }
    drag.frame = globalThis.requestAnimationFrame(() => {
      if (drag !== null) {
        drag.frame = null;
        step(false);
      }
    });
  }

  /**
   * The pointer's travel since pointer-down, in the canvas' own layout pixels.
   *
   * Raw client pixels: the DIVISOR is applied once, inside `draggedViewport`, so the zoom is not
   * divided twice (which is how a drag at 68% moved 1.47x too far the first time this was wired).
   */
  function travelOf(event: PointerEvent): { x: number; y: number } {
    if (drag === null) {
      return { x: 0, y: 0 };
    }
    return { x: event.clientX - drag.origin.x, y: event.clientY - drag.origin.y };
  }

  function onDown(event: PointerEvent, handle: DragHandle): void {
    if (!event.isPrimary || event.button !== 0) {
      return;
    }
    const from = options.current();
    drag = {
      pointerId: event.pointerId,
      handle,
      origin: { x: event.clientX, y: event.clientY },
      from,
      previous: { size: null, atMs: 0 },
      travel: { x: 0, y: 0 },
      frame: null,
    };
    // Capture on the grip: without it a fast drag off the grip delivers no moves and the size stops
    // at the first one. A synthetic pointer cannot be captured; the drag then still works while the
    // pointer stays on the grip.
    try {
      (event.currentTarget as Element | null)?.setPointerCapture?.(event.pointerId);
    } catch {
      // see above: the gesture degrades, it does not fail
    }
    setReadout(`${from.width}x${from.height}`);
    event.preventDefault();
    event.stopPropagation();
  }

  function onMove(event: PointerEvent): void {
    if (drag === null || event.pointerId !== drag.pointerId) {
      return;
    }
    // The readout follows every move; the ENGINE hears about it once per animation frame. That split
    // is the contract: the number tracks the pointer, and the preview lays out once per display
    // refresh rather than once per pointer event.
    drag.travel = travelOf(event);
    const preview = draggedViewport(drag.from, drag.handle, drag.travel, ratio());
    setReadout(resizeReadout(preview));
    schedule();
    event.preventDefault();
  }

  function onUp(event: PointerEvent): void {
    if (drag === null || event.pointerId !== drag.pointerId) {
      return;
    }
    cancelFrame();
    drag.travel = travelOf(event);
    step(true);
    try {
      (event.currentTarget as Element | null)?.releasePointerCapture?.(event.pointerId);
    } catch {
      // the browser already released it
    }
    drag = null;
    event.preventDefault();
  }

  /** The grip a press started on, or `null`. Read from the event's target, not looked up once. */
  function gripOf(target: EventTarget | null): HTMLElement | null {
    const element = target instanceof Element ? target.closest<HTMLElement>(".canvas-resize") : null;
    if (element === null || !RESIZE_HANDLES.some((handle) => handle.id === element.id)) {
      return null;
    }
    return element;
  }

  function handleOf(element: HTMLElement): DragHandle | null {
    return RESIZE_HANDLES.find((handle) => handle.id === element.id)?.handle ?? null;
  }

  // **Delegated, and on the document.** The session is created before React commits the grips (the
  // session is built from refs that exist at the first settle, and the grips are rendered with the
  // pane), so a listener looked up by id at construction finds nothing and the drag silently does
  // nothing. Delegation from the document with capture is also what makes the grip win the press
  // before the engine's pointer path sees it, without depending on where the grip sits in the tree.
  const onDocumentDown = (event: PointerEvent) => {
    const grip = gripOf(event.target);
    if (grip === null) {
      return;
    }
    const handle = handleOf(grip);
    if (handle !== null) {
      onDown(event, handle);
    }
  };
  document.addEventListener("pointerdown", onDocumentDown, { capture: true });
  cleanups.push(() => document.removeEventListener("pointerdown", onDocumentDown, { capture: true }));

  // Moves and the release follow the pointer, wherever it goes: capture keeps them on the grip, but a
  // browser that refuses the capture (a synthetic pointer) still delivers them here while the pointer
  // stays inside the canvas.
  document.addEventListener("pointermove", onMove, { capture: true });
  document.addEventListener("pointerup", onUp, { capture: true });
  document.addEventListener("pointercancel", onUp, { capture: true });
  cleanups.push(() => {
    document.removeEventListener("pointermove", onMove, { capture: true });
    document.removeEventListener("pointerup", onUp, { capture: true });
    document.removeEventListener("pointercancel", onUp, { capture: true });
  });

  return {
    dispose: () => {
      cancelFrame();
      drag = null;
      for (const cleanup of cleanups) {
        cleanup();
      }
      cleanups.length = 0;
    },
  };
}
