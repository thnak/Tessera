// SPDX-License-Identifier: MIT

// The Canvas view's style inspector (issue #171, designer-protocol-v1 section 9.1) and its write half
// (issue #585, sections 9.2-9.4 and 10).
//
// **What it is.** With the inspector armed, clicking a node in the canvas shows that node's computed
// style, the rules that matched it (highest precedence first, with the origin, the selector text,
// the specificity and each declaration's `overridden` flag), and its box. That is `style.info`.
//
// **The write half.** The same panel sets, clears and reverts a style override on the node
// (`canvas-inspector-style.ts`), and commits the node's overrides to the project: the commit round trip of
// section 10.3 is `lib/override-client.ts`, which asks the module for the `source.patch`, applies it to the
// file buffers through the page, and settles the overrides after the next evaluation. This file only wires
// the two to the panel and to the frame.
//
// **Why the panel is a page element and not engine paint.** The engine draws the previewed app and
// nothing else (ADR-054 decision 3). A panel drawn by the engine would be part of the frame, so it
// would have to be repainted on every selection and would contaminate the pixels a golden compares.
// Drawn on the page, it costs no frame at all: the query (`tessera_canvas_style`) dispatches
// nothing, and the panel is a `div` the browser composites. (An override DOES owe one frame: the layer
// restyles the running tree, and the session draws it.)
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
import type {
  CommitDestination,
  CommitHost,
  CommitOutcome,
  OverrideClient,
  OverrideDiagnostic,
  OverrideResult,
} from "../lib/override-client";
import { commitBlock, computedBlock, outcomeText, overridesBlock } from "./canvas-inspector-style";

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
  /** The override layer's client and the commit host (buffers, editor, evaluation), or `null` without a page. */
  overrides: OverrideClient;
  commitHost: () => CommitHost | null;
  /** The project's stylesheet paths, for the `class` destination. */
  stylesheets: () => string[];
  /** The logical viewport the running session was laid out at. */
  viewport: () => { width: number; height: number };
  /** An override operation or commit changed the tree: one frame is owed. */
  requestFrame: () => void;
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
  /** Commits the selected node's overrides to `destination` (the round trip of section 10.3). */
  commit(
    destination: CommitDestination,
    fields?: { className?: string; file?: string; token?: string; tokenValue?: string },
  ): Promise<CommitOutcome | null>;
  /** A commit finished outside `commit()` (a settle that waited for the source to compile). */
  showOutcome(outcome: CommitOutcome): void;
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
    return block.appendChild(row);
  };
  line("x y", [box.x, box.y, box.width, box.height]);
  line("margin", box.margin);
  line("border", box.border);
  line("padding", box.padding);
  return block;
}

function sectionWith(className: string, title: string, child: HTMLElement): HTMLElement {
  const block = document.createElement("div");
  block.className = className;
  const heading = document.createElement("div");
  heading.className = "inspect-section";
  heading.textContent = title;
  block.append(heading, child);
  return block;
}

/** What the page restores after the panel is rebuilt: the field that had focus, its text and caret. */
interface FocusMemory {
  field: string;
  value: string;
  start: number | null;
  end: number | null;
}

function rememberFocus(body: HTMLElement): FocusMemory | null {
  const active = document.activeElement;
  if (!(active instanceof HTMLInputElement) || !body.contains(active) || active.dataset.field === undefined) {
    return null;
  }
  return { field: active.dataset.field, value: active.value, start: active.selectionStart, end: active.selectionEnd };
}

function restoreFocus(body: HTMLElement, memory: FocusMemory | null): void {
  if (memory === null) {
    return;
  }
  const target = [...body.querySelectorAll<HTMLInputElement>("input[data-field]")].find(
    (candidate) => candidate.dataset.field === memory.field,
  );
  if (target === undefined) {
    return;
  }
  target.value = memory.value;
  target.focus();
  if (memory.start !== null && memory.end !== null && target.type === "text") {
    target.setSelectionRange(memory.start, memory.end);
  }
}

export function createCanvasInspector(options: CanvasInspectorOptions): CanvasInspector {
  const { panel, body, toggle, canvas } = options.elements;
  const setStatus = options.setStatus;
  const client = options.overrides;
  let isArmed = false;
  // The last node asked about, so a frame that moved the boxes can re-ask without a new click.
  let last: { clientX: number; clientY: number } | null = null;
  // The node the panel is showing, so a commit names the same node the panel describes even after
  // the pointer moved away. Zero means no node is selected.
  let selected = 0;
  let selectedLabel = "node";
  // The last commit's answer and the last refused value, kept so a re-render (a frame moved the boxes) does
  // not lose them.
  let lastOutcome: CommitOutcome | null = null;
  let refusal: OverrideDiagnostic[] | null = null;
  let lastReport: CanvasStyleReport | null = null;

  function hide(): void {
    panel.hidden = true;
    last = null;
    selected = 0;
    lastOutcome = null;
    refusal = null;
    lastReport = null;
  }

  function onResult(result: OverrideResult): void {
    // A refused value keeps its diagnostic next to the field; an accepted one clears it. Either way the layer
    // is the module's, so the panel re-asks it rather than patching its own copy.
    refusal = result.ok ? null : result.diagnostics;
    if (!result.ok) {
      setStatus(`override refused: ${result.diagnostics[0]?.code ?? result.reason ?? "unknown"}`, "warning");
    } else if (result.changed) {
      options.requestFrame();
    }
    refreshStyle();
  }

  function render(report: CanvasStyleReport): void {
    if (!report.ok) {
      // `no_such_node` is a real answer: the node the click named is gone (a rebuild replaced the
      // tree), so the panel hides rather than keeping the previous node's style under a new name.
      hide();
      return;
    }
    lastReport = report;
    const memory = rememberFocus(body);
    const sections: HTMLElement[] = [];
    const heading = document.createElement("div");
    heading.className = "inspect-heading";
    heading.textContent = `${selectedLabel} #${report.node ?? "0"}`;
    sections.push(heading);
    selected = Number(report.node ?? "0");

    const deps = {
      client,
      node: String(report.node ?? "0"),
      nodeLabel: selectedLabel,
      computed: report.computed ?? {},
      overrides: report.overrides ?? [],
      refusal,
      onResult,
    };
    // The write half sits first: it is what a designer came to the panel to do.
    sections.push(overridesBlock(deps));
    const computed = computedBlock(deps);
    if (computed !== null) {
      sections.push(computed);
    }

    const matched = report.matched ?? [];
    if (matched.length > 0) {
      const rules = document.createElement("div");
      for (const rule of matched) {
        rules.appendChild(matchedRule(rule));
      }
      sections.push(sectionWith("inspect-matched", "matched", rules));
    }
    if (report.box !== undefined) {
      sections.push(sectionWith("inspect-boxes", "box", boxBlock(report.box)));
    }

    // The commit action is last: a developer reads the style before deciding to commit it.
    const first = deps.overrides[0];
    sections.push(
      commitBlock({
        stylesheets: options.stylesheets(),
        tokenValue: first === undefined ? "" : (deps.computed[first.property] ?? first.value),
        hasOverrides: deps.overrides.length > 0,
        outcome: lastOutcome,
        onCommit: (destination, fields) => void commit(destination, fields),
      }),
    );

    body.replaceChildren(...sections);
    restoreFocus(body, memory);
    panel.hidden = false;
  }

  /** Re-asks `style.info` for the selected node (a query: no frame). */
  function refreshStyle(): void {
    const module = options.module();
    if (module === null || selected === 0) {
      return;
    }
    try {
      render(JSON.parse(callExport(module, "_tessera_canvas_style", [String(selected)], [])) as CanvasStyleReport);
    } catch (error) {
      setStatus("inspector unavailable: " + String(error), "warning");
    }
  }

  function showOutcome(outcome: CommitOutcome): void {
    lastOutcome = outcome;
    setStatus(outcomeText(outcome), outcome.status === "committed" ? "ok" : "warning");
    options.requestFrame();
    refreshStyle();
  }

  async function commit(
    destination: CommitDestination,
    fields: { className?: string; file?: string; token?: string; tokenValue?: string } = {},
  ): Promise<CommitOutcome | null> {
    const host = options.commitHost();
    if (host === null || selected === 0) {
      return null;
    }
    // The declarations a commit writes are the node's overrides (designer-protocol-v1 §10.1), which the
    // module holds in its override layer: the page names the node and the destination, never the values.
    const outcome = await client.commit(
      {
        node: String(selected),
        nodeLabel: selectedLabel,
        destination,
        className: fields.className,
        file: fields.file,
        token: fields.token,
        tokenValue: fields.tokenValue,
      },
      host,
    );
    showOutcome(outcome);
    return outcome;
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
    let picked: { found: boolean; automationId: string; elementKind?: string };
    try {
      picked = JSON.parse(callExport(module, "_tessera_canvas_pick", [], [x, y])) as typeof picked;
    } catch (error) {
      hide();
      setStatus("inspector unavailable: " + String(error), "warning");
      return false;
    }
    if (!picked.found) {
      hide();
      return false;
    }
    if (Number(picked.automationId) !== selected) {
      // A different node: its refusal and last commit answer are not this node's.
      refusal = null;
      lastOutcome = null;
    }
    selectedLabel = picked.elementKind === undefined || picked.elementKind === "" ? "node" : picked.elementKind;
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
    if (!isArmed) {
      return;
    }
    // The selected node survives a frame by id; the pointer position only matters for the first pick, and a
    // layout move would make a re-pick at the old point name a different node.
    if (selected !== 0) {
      refreshStyle();
    } else if (last !== null) {
      ask(last.clientX, last.clientY);
    }
  }

  toggle?.addEventListener("click", () => {
    setArmed(!isArmed);
  });

  // The overrides changed outside the panel (Edit > Undo, a settle): the rows follow.
  client.subscribe(() => {
    if (isArmed && selected !== 0 && lastReport !== null) {
      refreshStyle();
    }
  });

  return {
    armed: () => isArmed,
    setArmed,
    toggle: () => setArmed(!isArmed),
    click,
    hide,
    refresh,
    commit,
    showOutcome,
  };
}
