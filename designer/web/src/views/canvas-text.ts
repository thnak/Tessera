// SPDX-License-Identifier: MIT

// Typed text for the canvas view (issue #601, docs/design/canvas-text-input.md). A canvas cannot receive text
// by itself: there is no caret, no IME and no soft keyboard on a <canvas>. So while the engine says a
// TextInput or TextArea holds focus, this module puts one hidden <textarea> over that field and gives it the
// keyboard. The textarea is a text *source* only: it never shows or stores anything (it is kept empty), and
// every character, composition step and clipboard action it produces is forwarded to the engine through
// `tessera_canvas_text`, which owns the buffer, the caret, the selection and `onChange`.
//
// **What the engine owns.** Whether a field is focused (`textFocus` in every report), where it is
// (`fieldBox`, logical pixels), what an edit means, and whether a key was used (`handled`).
//
// **What the page owns.** The DOM focus, the IME candidate window (the textarea's position), and the browser
// events that carry text: `beforeinput`, the composition events, and `copy`/`cut`/`paste`.
//
// **Keyboard rules kept.** Printable keys are not preventDefault'd (the browser must produce `beforeinput`);
// an editing key is prevented only when the engine says it was `handled`. Tab and Shift+Tab are the scene's
// rules, as on the canvas: when the engine reports `left` the browser carries on, so the text field is never a
// keyboard trap. Ctrl/Cmd chords stay the browser's and the studio's, except Select all, Undo and Redo,
// which are the field's.

import { cssZoomOf } from "./canvas-zoom";

/** What the module reports for a text or key event; only the fields this module reads are named. */
export interface TextReport {
  changed?: boolean;
  handled?: boolean;
  left?: boolean;
  /** An editable text field holds focus. Absent means "not editing". */
  textFocus?: boolean;
  composing?: boolean;
  multiline?: boolean;
  /** UTF-8 byte offset of the caret in the field's value. */
  caret?: number;
  /** The field's box in logical viewport pixels. */
  fieldBox?: { x: number; y: number; width: number; height: number };
  /** The caret's painted rectangle in logical viewport pixels (where the IME window belongs). */
  caretBox?: { x: number; y: number; width: number; height: number };
  /** The selection a `copy` or `cut` returned. */
  text?: string;
}

export type TextKind = "commit" | "start" | "update" | "end" | "copy" | "cut";

export interface CanvasTextOptions {
  canvas: HTMLCanvasElement;
  /** Sends text to the module, or `null` when the view cannot take input. */
  sendText: (kind: TextKind, text: string) => TextReport | null;
  /** Sends a key to the module (the same call the canvas keyboard uses). */
  sendKey: (kind: "down" | "up" | "blur", key: string, modifiers: number, repeat: boolean) => TextReport | null;
  /** Layout CSS pixels per logical viewport pixel (the canvas's CSS width over the logical width). */
  cssPerLogical: () => number;
  /** The engine changed something that must be painted: request one frame. */
  onChanged: () => void;
}

export interface CanvasText {
  /** Feed the report of any pointer, key or text call: it shows, moves or hides the textarea. */
  sync: (report: TextReport | null) => void;
  /** True for the textarea: the canvas must not treat losing focus to it as losing the keyboard. */
  owns: (target: EventTarget | null) => boolean;
  /** True while focus is being handed back to the canvas, so its focus handler does not re-enter the tab order. */
  returning: () => boolean;
  dispose: () => void;
}

const SHIFT = 1;
const CONTROL = 2;
/** The chords the field owns; everything else with Ctrl/Cmd stays the browser's. */
const FIELD_CHORDS = new Set(["a", "z", "y"]);

/** The `ui::Modifier` mask for a key sent from the textarea, or `null` when the browser keeps the key. */
export function textModifiers(event: {
  key: string;
  shiftKey: boolean;
  ctrlKey: boolean;
  altKey: boolean;
  metaKey: boolean;
}): number | null {
  if (event.altKey) {
    return null;
  }
  const chord = event.ctrlKey || event.metaKey;
  if (chord) {
    // Cmd on a Mac is the engine's Control: one keymap for every platform.
    return FIELD_CHORDS.has(event.key.toLowerCase()) ? CONTROL | (event.shiftKey ? SHIFT : 0) : null;
  }
  return event.shiftKey ? SHIFT : 0;
}

/**
 * What the hidden textarea holds while a field is focused. Some soft keyboards send no key and no
 * `beforeinput` for Backspace in an empty textarea; with one character before the caret there is always
 * something to delete, so `deleteContentBackward` arrives and is forwarded as Backspace. A zero-width space is
 * invisible, never reaches the engine (every insert is prevented) and is restored after each commit.
 */
export const SENTINEL = "\u200B";

/** The delete a soft keyboard asks for in `beforeinput` when it sent no usable keydown (key 229). */
function deleteKey(inputType: string): string | null {
  switch (inputType) {
    case "deleteContentBackward":
      return "Backspace";
    case "deleteContentForward":
      return "Delete";
    default:
      return null;
  }
}

export function attachCanvasText(options: CanvasTextOptions): CanvasText {
  const { canvas } = options;
  const doc = canvas.ownerDocument;
  const host = canvas.parentElement;
  const area = doc.createElement("textarea");
  area.setAttribute("tabindex", "-1");
  area.setAttribute("autocomplete", "off");
  area.setAttribute("autocapitalize", "off");
  area.setAttribute("autocorrect", "off");
  area.setAttribute("spellcheck", "false");
  area.setAttribute("aria-label", "Text input for the canvas");
  area.setAttribute("data-canvas-text", "true");
  // Invisible but real: `display:none` and `visibility:hidden` cannot take focus. Transparent text and
  // caret keep a stray glyph off the canvas; the box sits over the field so the IME window opens there.
  Object.assign(area.style, {
    position: "absolute",
    opacity: "0",
    margin: "0",
    padding: "0",
    border: "0",
    outline: "none",
    resize: "none",
    overflow: "hidden",
    whiteSpace: "pre",
    background: "transparent",
    color: "transparent",
    caretColor: "transparent",
    pointerEvents: "none",
    display: "none",
  });
  host?.appendChild(area);

  let editing = false;
  let returning = false;
  let composing = false;
  let focusTimer: ReturnType<typeof setTimeout> | null = null;

  function place(report: TextReport): void {
    const box = report.fieldBox;
    if (box === undefined || host === null) {
      return;
    }
    // Same parent as the canvas, so the pane's transform scales both. The textarea is positioned in
    // layout pixels: the canvas's offset inside the host (undone from the zoom), plus the field's box
    // scaled from logical pixels.
    const canvasRect = canvas.getBoundingClientRect();
    const hostRect = host.getBoundingClientRect();
    const zoom = cssZoomOf(canvasRect.width, canvas.offsetWidth);
    const scale = options.cssPerLogical();
    const clamp = (value: number): number => (Number.isFinite(value) && value > 0 ? value : 1);
    const originX = (canvasRect.left - hostRect.left) / zoom;
    const originY = (canvasRect.top - hostRect.top) / zoom;
    const caret = report.caretBox;
    if (caret === undefined) {
      area.style.left = `${originX + box.x * scale}px`;
      area.style.top = `${originY + box.y * scale}px`;
      area.style.width = `${clamp(box.width * scale)}px`;
      area.style.height = `${clamp(box.height * scale)}px`;
      return;
    }
    // The IME opens its candidate window at the textarea's caret, which for an empty one is the start of
    // its first line. So the textarea is one caret-high line that begins at the engine's caret: the window
    // follows the insertion point instead of sitting at the field's corner (docs/design/canvas-text-input.md
    // section 8). It still ends at the field's right edge.
    const height = clamp(caret.height * scale);
    area.style.left = `${originX + caret.x * scale}px`;
    area.style.top = `${originY + caret.y * scale}px`;
    area.style.width = `${clamp((box.x + box.width - caret.x) * scale)}px`;
    area.style.height = `${height}px`;
    area.style.fontSize = `${height}px`;
    area.style.lineHeight = `${height}px`;
  }

  function reset(): void {
    // The textarea is a source, not a store: an empty box means nothing stale can be committed twice.
    area.value = editing ? SENTINEL : "";
    if (editing && typeof area.setSelectionRange === "function") {
      area.setSelectionRange(SENTINEL.length, SENTINEL.length);
    }
  }

  function show(report: TextReport): void {
    area.style.display = "block";
    place(report);
    if (!editing) {
      editing = true;
      reset();
      // A pointer press focuses the canvas as its default action, after this handler: focusing now
      // would be undone. A zero timeout lands after it.
      if (focusTimer !== null) {
        clearTimeout(focusTimer);
      }
      focusTimer = setTimeout(() => {
        focusTimer = null;
        if (editing) {
          area.focus({ preventScroll: true });
        }
      }, 0);
    }
  }

  function hide(refocus: boolean): void {
    if (!editing) {
      return;
    }
    editing = false;
    composing = false;
    if (focusTimer !== null) {
      clearTimeout(focusTimer);
      focusTimer = null;
    }
    reset(); // editing is false here: the sentinel goes with the field
    area.style.display = "none";
    if (refocus && doc.activeElement === area) {
      returning = true;
      canvas.focus({ preventScroll: true });
      returning = false;
    }
  }

  function sync(report: TextReport | null): void {
    if (report === null) {
      return;
    }
    if (report.textFocus === true) {
      show(report);
      return;
    }
    // The engine moved focus off the field. Tab that walked off the end leaves the page's control to the
    // browser (the caller handles it); anything else hands the keyboard back to the canvas.
    hide(report.left !== true);
  }

  function result(report: TextReport | null): TextReport | null {
    if (report?.changed === true) {
      options.onChanged();
    }
    sync(report);
    return report;
  }

  const onBeforeInput = (event: InputEvent): void => {
    if (event.isComposing || event.inputType === "insertCompositionText") {
      return; // the composition events own a preedit
    }
    const asKey = deleteKey(event.inputType);
    if (asKey !== null) {
      event.preventDefault();
      result(options.sendKey("down", asKey, 0, false));
      result(options.sendKey("up", asKey, 0, false));
      return;
    }
    let text: string | null = null;
    switch (event.inputType) {
      case "insertText":
      case "insertReplacementText":
        text = event.data ?? "";
        break;
      case "insertLineBreak":
      case "insertParagraph":
        text = "\n";
        break;
      case "insertFromPaste":
      case "insertFromDrop":
        text = event.data ?? event.dataTransfer?.getData("text/plain") ?? "";
        break;
      default:
        break;
    }
    if (text === null) {
      return;
    }
    event.preventDefault();
    if (text !== "") {
      result(options.sendText("commit", text));
    }
  };

  const onKey = (kind: "down" | "up") => (event: KeyboardEvent): void => {
    if (event.isComposing || event.keyCode === 229) {
      return; // an IME is reading the key
    }
    const modifiers = textModifiers(event);
    if (modifiers === null) {
      return;
    }
    const report = result(options.sendKey(kind, event.key, modifiers, event.repeat));
    if (report === null) {
      return;
    }
    if (report.handled === true) {
      event.preventDefault();
    }
    if (report.left === true && kind === "down" && event.shiftKey) {
      // Shift+Tab out of the field would land on the canvas, which re-enters its tab order on a keyboard
      // arrival. Skipping the canvas for one task lets the browser reach the element before it.
      const previous = canvas.getAttribute("tabindex");
      canvas.setAttribute("tabindex", "-1");
      setTimeout(() => {
        if (previous === null) {
          canvas.removeAttribute("tabindex");
        } else {
          canvas.setAttribute("tabindex", previous);
        }
      }, 0);
    }
  };
  const onKeyDown = onKey("down");
  const onKeyUp = onKey("up");

  const onCompositionStart = (): void => {
    composing = true;
    result(options.sendText("start", ""));
  };
  const onCompositionUpdate = (event: CompositionEvent): void => {
    result(options.sendText("update", event.data ?? ""));
  };
  const onCompositionEnd = (event: CompositionEvent): void => {
    composing = false;
    const data = event.data ?? "";
    // The result is committed text, which also closes the engine's composition; an empty result is a cancel.
    result(options.sendText(data === "" ? "end" : "commit", data));
    reset();
  };

  const onCopy = (event: ClipboardEvent): void => {
    const report = result(options.sendText("copy", ""));
    if (report?.text !== undefined && report.text !== "") {
      event.clipboardData?.setData("text/plain", report.text);
      event.preventDefault();
    }
  };
  const onCut = (event: ClipboardEvent): void => {
    const report = result(options.sendText("cut", ""));
    if (report?.text !== undefined && report.text !== "") {
      event.clipboardData?.setData("text/plain", report.text);
      event.preventDefault();
    }
  };
  const onPaste = (event: ClipboardEvent): void => {
    const text = event.clipboardData?.getData("text/plain") ?? "";
    event.preventDefault();
    if (text !== "") {
      result(options.sendText("commit", text));
    }
  };

  const onBlur = (event: FocusEvent): void => {
    if (!editing || returning) {
      return;
    }
    // Focus went to the canvas: the engine keeps the field focused and the canvas handles its own keys.
    if (event.relatedTarget === canvas) {
      return;
    }
    // Focus left for somewhere else on the page (or the window lost focus): the engine drops the field.
    // A composition in flight is cancelled by that, as a browser cancels one when its element blurs.
    if (composing) {
      composing = false;
    }
    result(options.sendKey("blur", "", 0, false));
    hide(false);
  };

  area.addEventListener("beforeinput", onBeforeInput as EventListener);
  area.addEventListener("keydown", onKeyDown as EventListener);
  area.addEventListener("keyup", onKeyUp as EventListener);
  area.addEventListener("compositionstart", onCompositionStart);
  area.addEventListener("compositionupdate", onCompositionUpdate as EventListener);
  area.addEventListener("compositionend", onCompositionEnd as EventListener);
  area.addEventListener("copy", onCopy as EventListener);
  area.addEventListener("cut", onCut as EventListener);
  area.addEventListener("paste", onPaste as EventListener);
  area.addEventListener("blur", onBlur as EventListener);

  return {
    sync,
    owns: (target) => target === area,
    returning: () => returning,
    dispose: () => {
      if (focusTimer !== null) {
        clearTimeout(focusTimer);
      }
      area.remove();
    },
  };
}
