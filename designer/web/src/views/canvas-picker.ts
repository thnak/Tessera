// SPDX-License-Identifier: MIT

// The Canvas view's picker overlay (issue #171's picker slice).
//
// **What it is.** While the picker is armed, moving the pointer over the canvas outlines the node
// under it and shows that node's identity — its element kind and automation id — in a small readout.
// It is the first thing a developer needs before a style inspector or `source.patch` can exist: you
// cannot edit what you cannot name.
//
// **Why the overlay is a page element and not engine paint.** The engine draws the previewed app and
// nothing else (ADR-054 decision 3). An outline drawn by the engine would be part of the frame, so it
// would have to be re-painted on every pointer move — the animation loop the canvas session exists to
// avoid — and it would contaminate the pixels a golden compares. Drawn on the page, it costs no frame
// at all: the pick is a query (`tessera_canvas_pick`) that dispatches nothing, and the outline is a
// `div` the browser composites.
//
// **Why `pointer-events: none` is load-bearing.** The overlay sits exactly over the canvas. If it
// accepted pointer events it would swallow every move and press, and the preview would stop
// responding the moment the picker was armed. The CSS rule is the contract; this module never adds a
// listener to the overlay itself.
//
// **The coordinate rule is the pointer rule.** The engine answers in logical viewport pixels, the
// same space `tessera_canvas_pointer` takes, so the page converts with the canvas' CSS-to-logical
// ratio — never the device pixel ratio (docs/design/canvas-input-session.md section 4). One helper,
// `toLogical`, is used in both directions so the outline cannot drift from the pointer that produced
// it.

import type { StatusReporter } from "../lib/views";
import { callExport, type WasmModule } from "../lib/wasm-call";

/** What one pick reports; the shape `canvas_pick_report_json()` writes. */
export interface CanvasPickReport {
  found: boolean;
  /** Decimal string, like every id in this module: a 64-bit id does not survive `JSON.parse`. */
  automationId: string;
  /** The node's element kind (`Button`, `Text`, `View`, ...); empty on a miss. */
  elementKind: string;
  /** The node's box in **logical** viewport pixels. */
  box: { x: number; y: number; width: number; height: number };
}

/** The elements the picker needs. All three are React refs, rendered childless. */
export interface CanvasPickerElements {
  /** The overlay: positioned over the canvas, `pointer-events: none`, hidden until a pick lands. */
  overlay: HTMLElement;
  /** The canvas, for its CSS box (the logical-to-CSS ratio) and its position. */
  canvas: HTMLCanvasElement;
  /** The pane's toggle. Null when the pane has no header, which the SSR chrome's pane does not. */
  toggle: HTMLButtonElement | null;
}

export interface CanvasPickerOptions {
  elements: CanvasPickerElements;
  /** The instantiated module, or null before it loads. */
  module: () => WasmModule | null;
  /** The logical viewport the running session was laid out at. */
  viewport: () => { width: number; height: number };  setStatus: StatusReporter;
}

export interface CanvasPicker {
  /** Whether the picker is armed. Off by default: the overlay is a tool, not the preview. */
  armed(): boolean;
  /** Arms or disarms the picker, hiding the overlay when it is disarmed. */
  setArmed(armed: boolean): void;
  /** Flips the picker; the toggle button's action. */
  toggle(): void;
  /** A pointer move over the canvas, in the event's client coordinates. */
  move(clientX: number, clientY: number): void;
  /** Hides the outline: the pointer left, or the tree changed under it. */
  hide(): void;
  /** Re-asks for the last point, after a frame moved the boxes. */
  refresh(): void;
}

/** The overlay's two children, created once and then moved. */
interface OverlayParts {
  outline: HTMLElement;
  readout: HTMLElement;
}

/**
 * The canvas' CSS-to-logical ratio and origin.
 *
 * `scale` is `logicalWidth / rect.width`, the same factor `tessera_canvas_pointer`'s contract tells
 * the page to divide by. It is 1 in the ordinary case (the canvas' CSS box is the logical viewport)
 * and differs only when a narrow pane shrinks the canvas through `max-width: 100%` — which is exactly
 * when a DPR-based conversion would be wrong.
 */
function canvasMapping(canvas: HTMLCanvasElement, logicalWidth: number): {
  rect: DOMRect;
  scale: number;
} {
  const rect = canvas.getBoundingClientRect();
  const scale = rect.width > 0 && logicalWidth > 0 ? logicalWidth / rect.width : 1;
  return { rect, scale };
}

/** One sentence for the readout: the element kind, then the id, or a named absence. */
function describe(report: CanvasPickReport): string {
  const kind = report.elementKind === "" ? "node" : report.elementKind;
  // `0` is the engine's "not set" (automation-ids-v1 section 1), so it is named rather than printed:
  // a readout saying `Button #0` would look like an id a developer could search for.
  return report.automationId === "0" ? `${kind} (no id)` : `${kind} #${report.automationId}`;
}

export function createCanvasPicker(options: CanvasPickerOptions): CanvasPicker {
  const { overlay, canvas, toggle } = options.elements;
  const setStatus = options.setStatus;
  let isArmed = false;
  // The last point asked about, so a frame that moved the boxes can re-ask without a new event.
  let last: { clientX: number; clientY: number } | null = null;
  let parts: OverlayParts | null = null;

  // The overlay's children are built here rather than by React: React rendered the overlay childless
  // once and never reconciles it again, which is the same rule the canvas itself follows. Building
  // them lazily keeps a page that never arms the picker free of two extra elements.
  function ensureParts(): OverlayParts {
    if (parts !== null) {
      return parts;
    }
    const outline = document.createElement("div");
    outline.className = "pick-outline";
    const readout = document.createElement("div");
    readout.className = "pick-readout";
    overlay.replaceChildren(outline, readout);
    parts = { outline, readout };
    return parts;
  }

  function hide(): void {
    overlay.hidden = true;
    last = null;
  }

  function show(report: CanvasPickReport): void {
    const { rect } = canvasMapping(canvas, options.viewport().width);
    const built = ensureParts();
    // The engine's box is logical; the outline is CSS, laid out inside the overlay. The pane may zoom the
    // stage with a CSS transform, which scales the overlay with the canvas, so the outline's own units are
    // the *layout* ones: logical width over `offsetWidth`, which a transform does not change. At 100% that
    // is the screen ratio `move()` uses, so the outline still lands under the pointer; zoomed, the
    // transform carries it there.
    const logicalWidth = options.viewport().width;
    const scale = canvas.offsetWidth > 0 && logicalWidth > 0 ? logicalWidth / canvas.offsetWidth : 1;
    const layoutWidth = canvas.offsetWidth > 0 ? canvas.offsetWidth : rect.width;
    const left = report.box.x / scale;
    const top = report.box.y / scale;
    const width = report.box.width / scale;
    const height = report.box.height / scale;
    built.outline.style.left = `${left}px`;
    built.outline.style.top = `${top}px`;
    built.outline.style.width = `${width}px`;
    built.outline.style.height = `${height}px`;
    built.readout.textContent = describe(report);
    // The readout sits just above the box, and flips below it when there is no room above: a label
    // that leaves the canvas is a label a developer cannot read on the node they are inspecting.
    const above = top - 22;
    built.readout.style.left = `${Math.max(0, Math.min(left, layoutWidth - 8))}px`;
    built.readout.style.top = `${above >= 0 ? above : top + height + 4}px`;
    overlay.hidden = false;
  }

  function move(clientX: number, clientY: number): void {
    if (!isArmed) {
      return;
    }
    const module = options.module();
    if (module === null) {
      // The module is not loaded yet: the picker is armed but has nothing to ask. Saying so once is
      // better than an overlay that silently never appears.
      hide();
      return;
    }
    last = { clientX, clientY };
    const { rect, scale } = canvasMapping(canvas, options.viewport().width);
    const x = (clientX - rect.left) * scale;
    const y = (clientY - rect.top) * scale;
    let report: CanvasPickReport;
    try {
      report = JSON.parse(callExport(module, "_tessera_canvas_pick", [], [x, y])) as CanvasPickReport;
    } catch (error) {
      // A module built before this export existed, or a malformed document: the overlay goes away and
      // the pane says why, rather than showing an outline for a node nobody asked about.
      hide();
      setStatus("picker unavailable: " + String(error), "warning");
      return;
    }
    if (!report.found) {
      // A point over no node is a real answer: the overlay hides instead of keeping the last outline,
      // which would claim the pointer is still over the node it left.
      hide();
      return;
    }
    show(report);
  }

  function setArmed(armed: boolean): void {
    isArmed = armed;
    if (toggle !== null) {
      toggle.setAttribute("aria-pressed", armed ? "true" : "false");
      toggle.textContent = armed ? "Picker on" : "Picker";
    }
    if (!armed) {
      hide();
      return;
    }
    setStatus("picker armed: move over the canvas", null);
  }

  function refresh(): void {
    if (!isArmed || last === null) {
      return;
    }
    move(last.clientX, last.clientY);
  }

  toggle?.addEventListener("click", () => {
    setArmed(!isArmed);
  });

  return {
    armed: () => isArmed,
    setArmed,
    toggle: () => setArmed(!isArmed),
    move,
    hide,
    refresh,
  };
}
