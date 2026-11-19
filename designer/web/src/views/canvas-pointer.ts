// SPDX-License-Identifier: MIT

// The canvas view's pointer input (issue #388, the page half): the browser's pointer events become calls to
// the module's `tessera_canvas_pointer`, and a change the engine reports becomes one frame.
//
// **What this file decides and what it does not.** It converts coordinates and forwards events; it never
// decides what a hover or a press *means*. `:hover`, `:active`, focus, capture and the `onClick` handler
// are the engine's (`docs/design/canvas-input-session.md` section 2): the router sets the pseudo-state on
// the node and the style engine matches `Button:hover`, so no class name is written from here.
//
// **The coordinate rule** (`tessera_canvas_pointer` in `tools/codegen/wasm_canvas_entry.cpp`): the module
// takes *logical* pixels, the layout viewport, which is the CSS box of the canvas. The conversion is the
// ratio of that viewport to the element's current CSS size -- 1 when the page shows the canvas at its
// logical size, and never `devicePixelRatio`. A 320x240 viewport at DPR 2 has a 640x480 backing store and
// a 320x240 CSS box, so a click at CSS (10, 10) is logical (10, 10); dividing by the DPR would hit-test
// half a button away.
//
// **Frames.** A move over nothing, or a press on a static scene, reports `changed: false` and costs
// nothing: no frame is requested. Only a change calls `onChanged`, and the caller's `requestFrame` already
// coalesces any number of them into one frame per display refresh -- the render-on-demand rule.

/** The four gestures the module's router knows (`pointer_kind_from`). */
export type PointerKind = "move" | "down" | "up" | "cancel";

/** What the module reports for one event; only `changed` decides whether a frame is owed. */
export interface PointerReport {
  changed: boolean;
  /** Handler ids the dispatch ran; a click that ran one rebuilt the tree under the pointer. */
  ran?: string[];
}

export interface CanvasPointerOptions {
  canvas: HTMLCanvasElement;
  /**
   * Sends one event to the module, or returns `null` when the view cannot take input (no device yet, a
   * hidden pane, a session that has not applied a project).
   */
  send: (kind: PointerKind, x: number, y: number, button: number) => PointerReport | null;
  /** The logical viewport the running session was laid out at (the toolbar's device). */
  viewport: () => { width: number; height: number };
  /** The engine changed something that must be painted: request one frame. */
  onChanged: () => void;
}

/**
 * Logical coordinates of a pointer event: the position inside the canvas' CSS box, scaled by the ratio of
 * the layout viewport to that box. An element with no size yet maps everything to (0, 0) rather than
 * dividing by zero.
 */
export function toLogical(
  event: { clientX: number; clientY: number },
  rect: { left: number; top: number; width: number; height: number },
  viewport: { width: number; height: number },
): { x: number; y: number } {
  if (rect.width <= 0 || rect.height <= 0) {
    return { x: 0, y: 0 };
  }
  return {
    x: (event.clientX - rect.left) * (viewport.width / rect.width),
    y: (event.clientY - rect.top) * (viewport.height / rect.height),
  };
}

/** Attaches the listeners; the returned function removes them. */
export function attachCanvasPointer(options: CanvasPointerOptions): () => void {
  const { canvas } = options;

  function forward(kind: PointerKind, event: PointerEvent): void {
    // Only the primary pointer drives hover and press (the router's rule): a second finger is ignored
    // here rather than being sent to be ignored there.
    if (!event.isPrimary) {
      return;
    }
    // The drawable box is inside the element's border: `clientLeft/Top/Width/Height` exclude it, which is
    // the box the layout viewport is shown in.
    const outer = canvas.getBoundingClientRect();
    const box = {
      left: outer.left + canvas.clientLeft,
      top: outer.top + canvas.clientTop,
      width: canvas.clientWidth,
      height: canvas.clientHeight,
    };
    const { x, y } = toLogical(event, box, options.viewport());
    const report = options.send(kind, x, y, event.button > 0 ? event.button : 0);
    if (report?.changed) {
      options.onChanged();
    }
    // A handler that ran changed state, and the engine rebuilds the tree for it: the router is reset with
    // the old tree, so the new button is no longer hovered although the pointer has not moved
    // (`docs/design/canvas-input-session.md`; `scene_canvas_pointer_tests.cpp` writes the rule down). The
    // page owns that: it sends the move again so the control under the pointer keeps its hover look. The
    // frame is the one already requested above, coalesced.
    if (kind === "up" && (report?.ran?.length ?? 0) > 0) {
      const again = options.send("move", x, y, 0);
      if (again?.changed) {
        options.onChanged();
      }
    }
  }

  const onMove = (event: PointerEvent): void => forward("move", event);
  const onDown = (event: PointerEvent): void => {
    // Capture keeps the press and the release on this element when the pointer is dragged off it, so the
    // `up` is delivered and `:active` cannot be left stuck on.
    if (event.isPrimary && typeof canvas.setPointerCapture === "function") {
      try {
        canvas.setPointerCapture(event.pointerId);
      } catch {
        // a synthetic or already-ended pointer cannot be captured; the press still goes through
      }
    }
    forward("down", event);
  };
  const onUp = (event: PointerEvent): void => forward("up", event);
  const onCancel = (event: PointerEvent): void => forward("cancel", event);
  // A real pointer leaving the scene: a move that hits nothing clears the hover, exactly what the router
  // does natively. Not while a button is held -- the capture above owns that gesture until it ends.
  const onLeave = (event: PointerEvent): void => {
    if (event.buttons !== 0 || !event.isPrimary) {
      return;
    }
    const report = options.send("move", -1, -1, 0);
    if (report?.changed) {
      options.onChanged();
    }
  };

  canvas.addEventListener("pointermove", onMove);
  canvas.addEventListener("pointerdown", onDown);
  canvas.addEventListener("pointerup", onUp);
  canvas.addEventListener("pointercancel", onCancel);
  canvas.addEventListener("pointerleave", onLeave);
  return () => {
    canvas.removeEventListener("pointermove", onMove);
    canvas.removeEventListener("pointerdown", onDown);
    canvas.removeEventListener("pointerup", onUp);
    canvas.removeEventListener("pointercancel", onCancel);
    canvas.removeEventListener("pointerleave", onLeave);
  };
}
