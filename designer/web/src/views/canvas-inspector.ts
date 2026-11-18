// SPDX-License-Identifier: MIT

// The Canvas view's style inspector (issue #171, designer-protocol-v1 section 9.1).
//
// **What it is.** With the inspector armed, clicking a node in the canvas shows that node's computed
// style, the rules that matched it (highest precedence first, with the origin, the selector text,
// the specificity and each declaration's `overridden` flag), and its box. It is the read half of
// `style.info`; the write half (`style.override.set`, `source.patch`) is a later slice.
//
// **Why the panel is a page element and not engine paint.** The engine draws the previewed app and
// nothing else (ADR-054 decision 3). A panel drawn by the engine would be part of the frame, so it
// would have to be repainted on every selection and would contaminate the pixels a golden compares.
// Drawn on the page, it costs no frame at all: the query (`tessera_canvas_style`) dispatches
// nothing, and the panel is a `div` the browser composites.
//
// **Why it is off by default.** The inspector is a tool, not the preview: an armed inspector
// consumes the click that would otherwise reach the app, so a visitor who never opens it keeps a
// fully interactive canvas. The toggle in the pane header is the only way in.
//
// **The coordinate rule is the picker's.** The engine answers the box in logical viewport pixels,
// the same space `tessera_canvas_pointer` takes, so the page converts with the canvas'
// CSS-to-logical ratio — never the device pixel ratio (docs/design/canvas-input-session.md §4).

import type { StatusReporter } from "../lib/views";
import { callExport, type WasmModule } from "../lib/wasm-call";

/** One declaration of one matched rule; the shape `canvas_style_report_json()` writes. */
export interface StyleDeclarationReport {
  property: string;
  value: string;
  important: boolean;
  /** True when a higher-precedence declaration won this property. */
  overridden: boolean;
}

/** One rule that matched the inspected node. */
export interface StyleMatchedRuleReport {
  /** `"inline"`, `"author"`, `"theme"` or `"ua"` (stylesheet-v1 section 7.1). */
  origin: string;
  selector: string;
  /** `[ids, classes, types]`. */
  specificity: [number, number, number];
  /** The rule's SourceSpan (designer-protocol-v1 section 3.1); `file` is empty for a blob rule. */
  span: { file: string; offset: number; length: number };
  declarations: StyleDeclarationReport[];
}

/** The node's box in logical pixels; the last three are top, right, bottom, left. */
export interface StyleBoxReport {
  x: number;
  y: number;
  width: number;
  height: number;
  margin: [number, number, number, number];
  border: [number, number, number, number];
  padding: [number, number, number, number];
}

/** What one `style.info` answers. A miss carries `ok:false` and `reason` and nothing else. */
export interface CanvasStyleReport {
  ok: boolean;
  /** `"no_such_node"` when `ok` is false. */
  reason?: string;
  /** Decimal string, like every id in this module. */
  node?: string;
  computed?: Record<string, string>;
  matched?: StyleMatchedRuleReport[];
  box?: StyleBoxReport;
  overrides?: { property: string; value: string }[];
}

/** The elements the inspector needs. */
export interface CanvasInspectorElements {
  /** The panel: a page element beside the canvas, hidden until a node is inspected. */
  panel: HTMLElement;
  /** The panel's body, which the inspector fills. */
  body: HTMLElement;
  /** The pane's toggle. Null when the pane has no header. */
  toggle: HTMLButtonElement | null;
  /** The canvas, for the CSS-to-logical ratio and the click position. */
  canvas: HTMLCanvasElement;
}

export interface CanvasInspectorOptions {
  elements: CanvasInspectorElements;
  /** The instantiated module, or null before it loads. */
  module: () => WasmModule | null;
  /** The logical viewport the running session was laid out at. */
  viewport: () => { width: number; height: number };
  setStatus: StatusReporter;
}

export interface CanvasInspector {
  /** Whether the inspector is armed. Off by default: it consumes the click it inspects. */
  armed(): boolean;
  /** Arms or disarms the inspector, hiding the panel when it is disarmed. */
  setArmed(armed: boolean): void;
  /** Flips the inspector; the toggle button's action. */
  toggle(): void;
  /** A click on the canvas, in the event's client coordinates. Returns true when it was consumed. */
  click(clientX: number, clientY: number): boolean;
  /** Hides the panel: the tree changed under it, or the view was left. */
  hide(): void;
  /** Re-asks for the last node, after a frame moved the boxes. */
  refresh(): void;
}

/** The canvas' CSS-to-logical ratio, the same conversion the picker and the pointer use. */
function canvasMapping(canvas: HTMLCanvasElement, logicalWidth: number): {
  rect: DOMRect;
  scale: number;
} {
  const rect = canvas.getBoundingClientRect();
  const scale = rect.width > 0 && logicalWidth > 0 ? logicalWidth / rect.width : 1;
  return { rect, scale };
}

/** `[ids, classes, types]` as the CSS spelling a developer reads: `0,1,0`. */
function specificityText(specificity: [number, number, number]): string {
  return specificity.join(",");
}

/** One row of the computed-style table. */
function computedRow(property: string, value: string): HTMLElement {
  const row = document.createElement("div");
  row.className = "inspect-row";
  const name = document.createElement("span");
  name.className = "inspect-name";
  name.textContent = property;
  const shown = document.createElement("span");
  shown.className = "inspect-value";
  shown.textContent = value;
  row.replaceChildren(name, shown);
  return row;
}

/**
 * One matched rule: its origin, selector, specificity and span, then its declarations.
 *
 * An overridden declaration is struck through rather than dropped: the point of the panel is to show
 * *why* a value is what it is, and a rule that lost is the answer to "why is my rule not applying".
 */
function matchedRule(rule: StyleMatchedRuleReport): HTMLElement {
  const block = document.createElement("div");
  block.className = "inspect-rule";
  block.dataset.origin = rule.origin;
  const header = document.createElement("div");
  header.className = "inspect-rule-header";
  const selector = document.createElement("code");
  selector.className = "inspect-selector";
  selector.textContent = rule.selector;
  const meta = document.createElement("span");
  meta.className = "inspect-meta";
  // The origin and the specificity are what a developer compares when two rules disagree; the span
  // is where the rule is, so a click can put a cursor there in a later slice.
  const where = rule.span.file === "" ? "" : ` ${rule.span.file}:${rule.span.offset}`;
  meta.textContent = `${rule.origin} · ${specificityText(rule.specificity)}${where}`;
  header.replaceChildren(selector, meta);
  const declarations = document.createElement("div");
  declarations.className = "inspect-declarations";
  for (const declaration of rule.declarations) {
    const row = document.createElement("div");
    row.className = declaration.overridden ? "inspect-decl overridden" : "inspect-decl";
    row.dataset.property = declaration.property;
    row.dataset.overridden = declaration.overridden ? "true" : "false";
    const name = document.createElement("span");
    name.className = "inspect-name";
    name.textContent = declaration.property;
    const value = document.createElement("span");
    value.className = "inspect-value";
    value.textContent = declaration.important ? `${declaration.value} !important` : declaration.value;
    row.replaceChildren(name, value);
    declarations.appendChild(row);
  }
  block.replaceChildren(header, declarations);
  return block;
}

/** The box, as one line of four numbers per edge set. */
function boxBlock(box: StyleBoxReport): HTMLElement {
  const block = document.createElement("div");
  block.className = "inspect-box";
  const line = (label: string, values: [number, number, number, number]): HTMLElement => {
    const row = document.createElement("div");
    row.className = "inspect-row";
    const name = document.createElement("span");
    name.className = "inspect-name";
    name.textContent = label;
    const shown = document.createElement("span");
    shown.className = "inspect-value";
    shown.textContent = values.join(" ");
    row.replaceChildren(name, shown);
    return row;
  };
  block.replaceChildren(
    line("x y", [box.x, box.y, box.width, box.height]),
    line("margin", box.margin),
    line("border", box.border),
    line("padding", box.padding),
  );
  return block;
}

export function createCanvasInspector(options: CanvasInspectorOptions): CanvasInspector {
  const { panel, body, toggle, canvas } = options.elements;
  const setStatus = options.setStatus;
  let isArmed = false;
  // The last node asked about, so a frame that moved the boxes can re-ask without a new click.
  let last: { clientX: number; clientY: number } | null = null;

  function hide(): void {
    panel.hidden = true;
    last = null;
  }

  function render(report: CanvasStyleReport): void {
    if (!report.ok) {
      // `no_such_node` is a real answer: the node the click named is gone (a rebuild replaced the
      // tree), so the panel hides rather than keeping the previous node's style under a new name.
      hide();
      return;
    }
    const sections: HTMLElement[] = [];
    const heading = document.createElement("div");
    heading.className = "inspect-heading";
    heading.textContent = `node #${report.node ?? "0"}`;
    sections.push(heading);

    const computed = report.computed ?? {};
    const computedNames = Object.keys(computed).sort();
    if (computedNames.length > 0) {
      const block = document.createElement("div");
      block.className = "inspect-computed";
      const title = document.createElement("div");
      title.className = "inspect-section";
      title.textContent = "computed";
      block.appendChild(title);
      for (const name of computedNames) {
        block.appendChild(computedRow(name, computed[name]));
      }
      sections.push(block);
    }

    const matched = report.matched ?? [];
    if (matched.length > 0) {
      const block = document.createElement("div");
      block.className = "inspect-matched";
      const title = document.createElement("div");
      title.className = "inspect-section";
      title.textContent = "matched";
      block.appendChild(title);
      for (const rule of matched) {
        block.appendChild(matchedRule(rule));
      }
      sections.push(block);
    }

    if (report.box !== undefined) {
      const block = document.createElement("div");
      block.className = "inspect-boxes";
      const title = document.createElement("div");
      title.className = "inspect-section";
      title.textContent = "box";
      block.appendChild(title);
      block.appendChild(boxBlock(report.box));
      sections.push(block);
    }

    body.replaceChildren(...sections);
    panel.hidden = false;
  }

  function ask(clientX: number, clientY: number): boolean {
    const module = options.module();
    if (module === null) {
      hide();
      return false;
    }
    const { rect, scale } = canvasMapping(canvas, options.viewport().width);
    const x = (clientX - rect.left) * scale;
    const y = (clientY - rect.top) * scale;
    // The click names a node through the picker's query, which is the same hit test the router uses:
    // the node the inspector describes is the node a press at the same point would reach.
    let picked: { found: boolean; automationId: string };
    try {
      picked = JSON.parse(callExport(module, "_tessera_canvas_pick", [], [x, y])) as {
        found: boolean;
        automationId: string;
      };
    } catch (error) {
      hide();
      setStatus("inspector unavailable: " + String(error), "warning");
      return false;
    }
    if (!picked.found) {
      hide();
      return false;
    }
    let report: CanvasStyleReport;
    try {
      report = JSON.parse(
        callExport(module, "_tessera_canvas_style", [picked.automationId], []),
      ) as CanvasStyleReport;
    } catch (error) {
      hide();
      setStatus("inspector unavailable: " + String(error), "warning");
      return false;
    }
    render(report);
    return report.ok;
  }

  function click(clientX: number, clientY: number): boolean {
    if (!isArmed) {
      return false;
    }
    last = { clientX, clientY };
    return ask(clientX, clientY);
  }

  function setArmed(armed: boolean): void {
    isArmed = armed;
    if (toggle !== null) {
      toggle.setAttribute("aria-pressed", armed ? "true" : "false");
      toggle.textContent = armed ? "Inspector on" : "Inspector";
    }
    if (!armed) {
      hide();
      return;
    }
    setStatus("inspector armed: click a node", null);
  }

  function refresh(): void {
    if (!isArmed || last === null) {
      return;
    }
    ask(last.clientX, last.clientY);
  }

  toggle?.addEventListener("click", () => {
    setArmed(!isArmed);
  });

  return {
    armed: () => isArmed,
    setArmed,
    toggle: () => setArmed(!isArmed),
    click,
    hide,
    refresh,
  };
}
