// SPDX-License-Identifier: MIT

// The canvas view's keyboard input (issue #388, focus): the canvas element is a focus stop, its key events
// become `tessera_canvas_key` calls, and the engine decides what they mean.
//
// **What the engine owns.** Which control holds focus, whether the `:focus-visible` ring shows, what Tab and
// Shift+Tab do and what Enter and Space activate are the scene's rules (`FocusManager`, the router's click
// synthesis), not this file's. The page forwards the key and obeys two answers.
//
//   * `handled` -- the scene used the key (it moved focus, activated a control, or reached a handler), so the
//     page calls `preventDefault`. Anything else is left to the browser.
//   * `left` -- Tab (or Shift+Tab) walked off the end of the tab order. The engine released focus and the
//     page does **not** prevent the default, so the browser carries on to the next element: a canvas that
//     swallowed Tab would be a keyboard trap.
//
// **Modifier keys stay the page's.** A chord with Ctrl, Alt or Meta is not forwarded: it is the browser's or the
// studio's (Ctrl+S, Ctrl+Z, a screen reader's keys), and a scene control has no use for it in this slice.
//
// **Entering and leaving.** Tabbing onto the canvas from outside focuses its first control, so the ring
// appears on arrival instead of after a second press; a click focuses the control under the pointer (the
// router does that, without the ring). When the element loses the keyboard the engine drops focus and
// the ring, so an unfocused canvas never looks focused.

import type { PointerReport } from "./canvas-pointer";

/** What the module reports for one key event (the pointer report plus the two keyboard answers). */
export interface KeyReport extends PointerReport {
  /** The scene consumed the key: the page prevents the browser's default. */
  handled?: boolean;
  /** Tab walked off the end of the tab order; focus was released and the browser should carry on. */
  left?: boolean;
}

export type KeyKind = "down" | "up" | "blur";

export interface CanvasKeyboardOptions {
  canvas: HTMLCanvasElement;
  /** Sends one event to the module, or `null` when the view cannot take input (no device, hidden pane). */
  send: (kind: KeyKind, key: string, modifiers: number, repeat: boolean) => KeyReport | null;
  /** The engine changed something that must be painted: request one frame. */
  onChanged: () => void;
}

/** The `ui::Modifier` mask the module takes: Shift 1, Control 2, Alt 4, Meta 8. */
export function modifierMask(event: { shiftKey: boolean; ctrlKey: boolean; altKey: boolean; metaKey: boolean }): number {
  return (event.shiftKey ? 1 : 0) | (event.ctrlKey ? 2 : 0) | (event.altKey ? 4 : 0) | (event.metaKey ? 8 : 0);
}

/** Attaches the listeners; the returned function removes them. */
export function attachCanvasKeyboard(options: CanvasKeyboardOptions): () => void {
  const { canvas } = options;

  function forward(kind: KeyKind, event: KeyboardEvent): void {
    // Chords with Ctrl/Alt/Meta and IME composition belong to the browser and the studio.
    if (event.ctrlKey || event.altKey || event.metaKey || event.isComposing) {
      return;
    }
    const report = options.send(kind, event.key, modifierMask(event), event.repeat);
    if (report === null) {
      return;
    }
    if (report.handled) {
      event.preventDefault();
    }
    if (report.changed) {
      options.onChanged();
    }
  }

  const onDown = (event: KeyboardEvent): void => forward("down", event);
  const onUp = (event: KeyboardEvent): void => forward("up", event);
  const onFocus = (): void => {
    // Keyboard arrival only: a mouse click focuses the element too, and the router focuses the control
    // under the pointer itself. `:focus-visible` is the browser's own keyboard-modality test.
    let keyboard = false;
    try {
      keyboard = canvas.matches(":focus-visible");
    } catch {
      keyboard = false;
    }
    if (!keyboard) {
      return;
    }
    const report = options.send("down", "Tab", 0, false);
    if (report?.changed) {
      options.onChanged();
    }
  };
  const onBlur = (): void => {
    const report = options.send("blur", "", 0, false);
    if (report?.changed) {
      options.onChanged();
    }
  };

  canvas.addEventListener("keydown", onDown);
  canvas.addEventListener("keyup", onUp);
  canvas.addEventListener("focus", onFocus);
  canvas.addEventListener("blur", onBlur);
  return () => {
    canvas.removeEventListener("keydown", onDown);
    canvas.removeEventListener("keyup", onUp);
    canvas.removeEventListener("focus", onFocus);
    canvas.removeEventListener("blur", onBlur);
  };
}
