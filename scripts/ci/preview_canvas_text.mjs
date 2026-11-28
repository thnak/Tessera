// SPDX-License-Identifier: MIT
//
// The canvas's typed-text bridge (issue #601, docs/design/canvas-text-input.md), tested in plain Node
// against a small fake DOM: the hidden textarea that takes the keyboard while the engine reports a focused
// TextInput, and what it forwards to `tessera_canvas_text` / `tessera_canvas_key`. The engine side is covered
// by `scene_canvas_text_tests`; the real-browser behavior (an actual IME) is recorded by the PR.
//
//   node scripts/ci/preview_canvas_text.mjs
//
// Needs the SSR build (`npm run build:chrome --prefix designer/web`).

import assert from "node:assert/strict";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const DIST = resolve(HERE, "..", "..", "designer", "web", "dist-ssr");

let failures = 0;
const queue = [];
function test(name, body) {
  queue.push({ name, body });
}

let textModule;
let keyboardModule;
try {
  textModule = await import(pathToFileURL(resolve(DIST, "canvas-text.js")).href);
  keyboardModule = await import(pathToFileURL(resolve(DIST, "canvas-keyboard.js")).href);
} catch {
  process.stderr.write(
    `SETUP ERROR: ${DIST}/canvas-text.js is missing. Build the page first:\n` +
      "  npm ci --prefix designer/web && npm run build --prefix designer/web\n",
  );
  process.exit(2);
}
const { attachCanvasText, textModifiers } = textModule;
const { attachCanvasKeyboard } = keyboardModule;

// --- a fake DOM, just enough for the two modules -------------------------------------------------------

class FakeElement {
  constructor(doc, tag) {
    this.ownerDocument = doc;
    this.tag = tag;
    this.attributes = new Map();
    this.style = {};
    this.children = [];
    this.parentElement = null;
    this.listeners = new Map();
    this.value = "";
    this.offsetWidth = 0;
    this.rect = { left: 0, top: 0, width: 0, height: 0 };
    this.focusVisible = false;
  }
  setAttribute(name, value) {
    this.attributes.set(name, String(value));
  }
  getAttribute(name) {
    return this.attributes.has(name) ? this.attributes.get(name) : null;
  }
  removeAttribute(name) {
    this.attributes.delete(name);
  }
  appendChild(child) {
    child.parentElement = this;
    this.children.push(child);
    return child;
  }
  remove() {
    if (this.parentElement) {
      this.parentElement.children = this.parentElement.children.filter((c) => c !== this);
      this.parentElement = null;
    }
  }
  addEventListener(type, handler) {
    const list = this.listeners.get(type) ?? [];
    list.push(handler);
    this.listeners.set(type, list);
  }
  removeEventListener(type, handler) {
    this.listeners.set(type, (this.listeners.get(type) ?? []).filter((h) => h !== handler));
  }
  getBoundingClientRect() {
    return { ...this.rect, right: this.rect.left + this.rect.width, bottom: this.rect.top + this.rect.height };
  }
  matches() {
    return this.focusVisible;
  }
  focus() {
    const previous = this.ownerDocument.activeElement;
    if (previous === this) {
      return;
    }
    this.ownerDocument.activeElement = this;
    if (previous) {
      previous.dispatch("blur", { relatedTarget: this });
    }
    this.dispatch("focus", { relatedTarget: previous });
  }
  dispatch(type, init = {}) {
    const event = {
      type,
      target: this,
      defaultPrevented: false,
      preventDefault() {
        this.defaultPrevented = true;
      },
      ...init,
    };
    for (const handler of this.listeners.get(type) ?? []) {
      handler(event);
    }
    return event;
  }
}

function makeDom() {
  const doc = { activeElement: null };
  doc.createElement = (tag) => new FakeElement(doc, tag);
  const host = new FakeElement(doc, "div");
  const canvas = new FakeElement(doc, "canvas");
  host.appendChild(canvas);
  canvas.offsetWidth = 320;
  canvas.rect = { left: 10, top: 20, width: 320, height: 240 };
  host.rect = { left: 0, top: 0, width: 400, height: 300 };
  canvas.setAttribute("tabindex", "0");
  return { doc, host, canvas };
}

const tick = () => new Promise((done) => setTimeout(done, 5));

/** A wired text bridge with a scripted engine: `engine.nextText`/`nextKey` are the reports it answers. */
function setup() {
  const dom = makeDom();
  const engine = { texts: [], keys: [], changed: 0, nextText: {}, nextKey: {} };
  const text = attachCanvasText({
    canvas: dom.canvas,
    cssPerLogical: () => 1,
    onChanged: () => {
      engine.changed += 1;
    },
    sendText: (kind, value) => {
      engine.texts.push([kind, value]);
      return engine.nextText;
    },
    sendKey: (kind, key, modifiers, repeat) => {
      engine.keys.push([kind, key, modifiers, repeat]);
      return engine.nextKey;
    },
  });
  const area = dom.host.children.find((c) => c.tag === "textarea");
  const focused = { textFocus: true, fieldBox: { x: 8, y: 16, width: 180, height: 28 } };
  return { ...dom, engine, text, area, focused };
}

test("the textarea exists next to the canvas, hidden, and is not a tab stop", () => {
  const { area, canvas } = setup();
  assert.ok(area, "a textarea was added beside the canvas");
  assert.equal(area.style.display, "none");
  assert.equal(area.getAttribute("tabindex"), "-1");
  assert.equal(area.style.pointerEvents, "none");
  assert.equal(canvas.getAttribute("tabindex"), "0", "the canvas stays the focus stop");
});

test("a focused field shows the textarea over it, scaled, and gives it the keyboard", async () => {
  const dom = setup();
  dom.text.sync(dom.focused);
  assert.equal(dom.area.style.display, "block");
  // canvas offset (10, 20) inside the host at zoom 1, plus the field's box at 1 css px per logical px.
  assert.equal(dom.area.style.left, "18px");
  assert.equal(dom.area.style.top, "36px");
  assert.equal(dom.area.style.width, "180px");
  assert.equal(dom.area.style.height, "28px");
  assert.notEqual(dom.doc.activeElement, dom.area, "focus waits one task so a press cannot take it back");
  await tick();
  assert.equal(dom.doc.activeElement, dom.area);
});

test("under a pane zoom the position is in layout pixels, not screen pixels", () => {
  const dom = makeDom();
  // The pane scales the stage by 0.5: the canvas's rect is half its layout width.
  dom.canvas.rect = { left: 105, top: 60, width: 160, height: 120 };
  dom.host.rect = { left: 100, top: 50, width: 200, height: 150 };
  const text = attachCanvasText({
    canvas: dom.canvas,
    cssPerLogical: () => 2,
    onChanged: () => {},
    sendText: () => ({}),
    sendKey: () => ({}),
  });
  const area = dom.host.children.find((c) => c.tag === "textarea");
  text.sync({ textFocus: true, fieldBox: { x: 5, y: 7, width: 10, height: 4 } });
  // (105 - 100) / 0.5 = 10 layout px of offset, plus 5 logical * 2 css px per logical.
  assert.equal(area.style.left, "20px");
  assert.equal(area.style.top, "34px");
  assert.equal(area.style.width, "20px");
});

test("with a caret box the textarea is one caret-high line starting at the caret (the IME window)", () => {
  const dom = setup();
  dom.text.sync({ ...dom.focused, caretBox: { x: 60, y: 19, width: 1, height: 22 } });
  // canvas offset (10, 20) plus the caret at 1 css px per logical px; it ends at the field's right edge (8 + 180).
  assert.equal(dom.area.style.left, "70px");
  assert.equal(dom.area.style.top, "39px");
  assert.equal(dom.area.style.width, "128px");
  assert.equal(dom.area.style.height, "22px");
  assert.equal(dom.area.style.lineHeight, "22px", "one line, so the candidate window sits under the caret");
  dom.text.sync({ ...dom.focused, caretBox: { x: 100, y: 19, width: 1, height: 22 } });
  assert.equal(dom.area.style.left, "110px", "it follows the caret as it moves");
});

test("typed text is committed and its default is prevented", () => {
  const { text, area, engine, focused } = setup();
  text.sync(focused);
  const event = area.dispatch("beforeinput", { inputType: "insertText", data: "a", isComposing: false });
  assert.deepEqual(engine.texts, [["commit", "a"]]);
  assert.equal(event.defaultPrevented, true);
  assert.deepEqual(engine.keys, [], "no key was sent for text");
});

test("a line break is committed as a newline, a soft-keyboard delete as Backspace", () => {
  const { text, area, engine, focused } = setup();
  text.sync(focused);
  area.dispatch("beforeinput", { inputType: "insertLineBreak", isComposing: false });
  assert.deepEqual(engine.texts, [["commit", "\n"]]);
  area.dispatch("beforeinput", { inputType: "deleteContentBackward", isComposing: false });
  assert.deepEqual(engine.keys, [
    ["down", "Backspace", 0, false],
    ["up", "Backspace", 0, false],
  ]);
});

test("an editing key is prevented only when the engine handled it", () => {
  const { text, area, engine, focused } = setup();
  text.sync(focused);
  engine.nextKey = { handled: true, textFocus: true, fieldBox: focused.fieldBox };
  const back = area.dispatch("keydown", { key: "Backspace", keyCode: 8, shiftKey: false, ctrlKey: false, altKey: false, metaKey: false, repeat: false, isComposing: false });
  assert.equal(back.defaultPrevented, true);
  engine.nextKey = { handled: false, textFocus: true, fieldBox: focused.fieldBox };
  const letter = area.dispatch("keydown", { key: "a", keyCode: 65, shiftKey: false, ctrlKey: false, altKey: false, metaKey: false, repeat: false, isComposing: false });
  assert.equal(letter.defaultPrevented, false, "a printable key is left to the browser so beforeinput fires");
  assert.deepEqual(engine.keys.map((k) => k[1]), ["Backspace", "a"]);
});

test("only Select all, Undo and Redo chords are the field's; Cmd is Control", () => {
  const base = { shiftKey: false, ctrlKey: false, altKey: false, metaKey: false };
  assert.equal(textModifiers({ ...base, key: "a", ctrlKey: true }), 2);
  assert.equal(textModifiers({ ...base, key: "z", metaKey: true, shiftKey: true }), 3);
  assert.equal(textModifiers({ ...base, key: "y", ctrlKey: true }), 2);
  assert.equal(textModifiers({ ...base, key: "s", ctrlKey: true }), null, "Save stays the studio's");
  assert.equal(textModifiers({ ...base, key: "r", metaKey: true }), null);
  assert.equal(textModifiers({ ...base, key: "a", altKey: true }), null);
  assert.equal(textModifiers({ ...base, key: "ArrowLeft", shiftKey: true }), 1);
  const { text, area, engine, focused } = setup();
  text.sync(focused);
  area.dispatch("keydown", { key: "c", keyCode: 67, ctrlKey: true, shiftKey: false, altKey: false, metaKey: false, repeat: false, isComposing: false });
  assert.deepEqual(engine.keys, [], "Ctrl+C is the clipboard event's, not a key");
});

test("composition: start, update, then a commit or a cancel; keys during it are the IME's", () => {
  const { text, area, engine, focused } = setup();
  text.sync(focused);
  area.dispatch("compositionstart", {});
  area.dispatch("compositionupdate", { data: "ni" });
  area.dispatch("keydown", { key: "n", keyCode: 229, isComposing: true, shiftKey: false, ctrlKey: false, altKey: false, metaKey: false, repeat: false });
  area.dispatch("beforeinput", { inputType: "insertCompositionText", data: "ni", isComposing: true });
  assert.deepEqual(engine.keys, [], "the IME's keys are not forwarded");
  area.value = "ni";
  area.dispatch("compositionend", { data: "\u4f60" });
  assert.deepEqual(engine.texts, [["start", ""], ["update", "ni"], ["commit", "\u4f60"]]);
  assert.equal(area.value, "", "the textarea is a source: it is emptied after a commit");
  engine.texts.length = 0;
  area.dispatch("compositionstart", {});
  area.dispatch("compositionupdate", { data: "zz" });
  area.dispatch("compositionend", { data: "" });
  assert.deepEqual(engine.texts, [["start", ""], ["update", "zz"], ["end", ""]]);
});

test("paste commits the clipboard text, copy and cut put the engine's selection on the clipboard", () => {
  const { text, area, engine, focused } = setup();
  text.sync(focused);
  const written = [];
  const clipboardData = { getData: () => "pasted", setData: (type, value) => written.push([type, value]) };
  const paste = area.dispatch("paste", { clipboardData });
  assert.deepEqual(engine.texts, [["commit", "pasted"]]);
  assert.equal(paste.defaultPrevented, true);
  engine.nextText = { handled: true, text: "hello", textFocus: true, fieldBox: focused.fieldBox };
  const copy = area.dispatch("copy", { clipboardData });
  const cut = area.dispatch("cut", { clipboardData });
  assert.deepEqual(written, [["text/plain", "hello"], ["text/plain", "hello"]]);
  assert.equal(copy.defaultPrevented && cut.defaultPrevented, true);
  assert.deepEqual(engine.texts.slice(1).map((t) => t[0]), ["copy", "cut"]);
  engine.nextText = { textFocus: true, fieldBox: focused.fieldBox };
  const empty = area.dispatch("copy", { clipboardData });
  assert.equal(empty.defaultPrevented, false, "nothing selected: the browser's own copy is left alone");
});

test("a changed report asks for one frame", () => {
  const { text, area, engine, focused } = setup();
  text.sync(focused);
  engine.nextText = { changed: true, textFocus: true, fieldBox: focused.fieldBox };
  area.dispatch("beforeinput", { inputType: "insertText", data: "x", isComposing: false });
  assert.equal(engine.changed, 1);
});

test("the engine moving focus off the field hands the keyboard back to the canvas", async () => {
  const dom = setup();
  dom.text.sync(dom.focused);
  await tick();
  assert.equal(dom.doc.activeElement, dom.area);
  let canvasArrivals = 0;
  dom.canvas.addEventListener("focus", () => {
    canvasArrivals += dom.text.returning() ? 0 : 1;
  });
  dom.text.sync({ textFocus: undefined });
  assert.equal(dom.area.style.display, "none");
  assert.equal(dom.doc.activeElement, dom.canvas, "focus returns to the canvas");
  assert.equal(canvasArrivals, 0, "and the canvas knows it is a return, not a keyboard arrival");
});

test("Tab off the end of the order leaves the page alone: not a keyboard trap", async () => {
  const dom = setup();
  dom.text.sync(dom.focused);
  await tick();
  dom.engine.nextKey = { left: true };
  const event = dom.area.dispatch("keydown", { key: "Tab", keyCode: 9, shiftKey: false, ctrlKey: false, altKey: false, metaKey: false, repeat: false, isComposing: false });
  assert.equal(event.defaultPrevented, false, "the browser carries on to the next element");
  assert.equal(dom.area.style.display, "none");
  assert.notEqual(dom.doc.activeElement, dom.canvas, "the canvas does not take the focus back");
});

test("Shift+Tab out of the field skips the canvas for one task, then restores its tab stop", async () => {
  const dom = setup();
  dom.text.sync(dom.focused);
  await tick();
  dom.engine.nextKey = { left: true };
  dom.area.dispatch("keydown", { key: "Tab", keyCode: 9, shiftKey: true, ctrlKey: false, altKey: false, metaKey: false, repeat: false, isComposing: false });
  assert.equal(dom.canvas.getAttribute("tabindex"), "-1");
  await tick();
  assert.equal(dom.canvas.getAttribute("tabindex"), "0");
});

test("focus leaving for the page drops the field; focus moving to the canvas keeps it", async () => {
  const dom = setup();
  dom.text.sync(dom.focused);
  await tick();
  dom.area.dispatch("blur", { relatedTarget: dom.canvas });
  assert.deepEqual(dom.engine.keys, [], "a hand-off to the canvas is not a blur");
  dom.area.dispatch("blur", { relatedTarget: null });
  assert.deepEqual(dom.engine.keys, [["blur", "", 0, false]]);
  assert.equal(dom.area.style.display, "none");
});

test("the canvas does not drop the field when the keyboard moves to the textarea", () => {
  const dom = makeDom();
  const sent = [];
  const owner = { owns: (target) => target === "the-textarea", returning: () => false };
  attachCanvasKeyboard({ canvas: dom.canvas, onChanged() {}, text: owner, send: (...args) => { sent.push(args); return { changed: false }; } });
  dom.canvas.dispatch("blur", { relatedTarget: "the-textarea" });
  assert.deepEqual(sent, [], "no blur for the engine");
  dom.canvas.dispatch("blur", { relatedTarget: null });
  assert.equal(sent.length, 1);
  assert.equal(sent[0][0], "blur");
});

test("taking the focus back is not a keyboard arrival (it would re-enter the tab order)", () => {
  const dom = makeDom();
  const sent = [];
  dom.canvas.focusVisible = true;
  let returning = true;
  attachCanvasKeyboard({ canvas: dom.canvas, onChanged() {}, text: { owns: () => false, returning: () => returning }, send: (...args) => { sent.push(args); return { changed: false }; } });
  dom.canvas.dispatch("focus", {});
  assert.deepEqual(sent, []);
  returning = false;
  dom.canvas.dispatch("focus", {});
  assert.equal(sent.length, 1, "a real keyboard arrival still sends Tab");
  assert.deepEqual(sent[0].slice(0, 2), ["down", "Tab"]);
});

for (const { name, body } of queue) {
  try {
    await body();
    process.stdout.write(`ok   ${name}\n`);
  } catch (error) {
    failures += 1;
    process.stdout.write(`FAIL ${name}\n     ${error && error.message ? error.message : error}\n`);
  }
}

process.stdout.write(failures === 0 ? "\ncanvas text: all passed\n" : `\ncanvas text: ${failures} failed\n`);
if (failures > 0) {
  process.exitCode = 1;
}
