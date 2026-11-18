// SPDX-License-Identifier: MIT

// The Canvas view's style inspector (issue #171, designer-protocol-v1 section 9.1).
//
// **What it is.** With the inspector armed, clicking a node in the canvas shows that node's computed
// style, the rules that matched it (highest precedence first, with the origin, the selector text,
// the specificity and each declaration's `overridden` flag), and its box. It is the read half of
// `style.info`.
//
// **The commit action is the write half's first step, and only the first step.** Pressing a
// destination button asks the engine for the `source.patch` that commit would produce
// (designer-protocol-v1 §10.2) and shows the resulting edits, or the spec's reason there are none.
// It does **not** write the file: applying a patch is the host's job (§10.3), and this page has no
// host. The panel says "produced — not applied" for exactly that reason; a button that claimed the
// file was written would be lying about the developer's source.
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

/** One edit of a `source.patch`; the shape `canvas_patch_report_json()` writes. */
export interface PatchEditReport {
  file: string;
  /** Byte offset into the file; on a UTF-8 code point boundary (designer-protocol-v1 §3.3). */
  offset: number;
  /** Bytes replaced; 0 inserts. */
  length: number;
  newText: string;
}

/**
 * What one commit produced (designer-protocol-v1 §10.2).
 *
 * **`ok:false` carries no `edits` and no `baseVersions`**, because the spec sends no `source.patch`
 * for a failed commit. The panel shows the `reason` and says nothing was produced.
 */
export interface CanvasPatchReport {
  ok: boolean;
  /** One of the spec's six reasons when `ok` is false. */
  reason?: string;
  /** A human-readable detail for the status line; never part of the protocol's `reason`. */
  detail?: string;
  /** The viewer's handle for this patch, which the host's `source.patch` names. */
  patchId?: string;
  /** Sorted by file and descending offset, so applying in order never shifts a later edit. */
  edits?: PatchEditReport[];
  /** Each affected file path to the version the viewer used. */
  baseVersions?: Record<string, number>;
}

/** The three commit destinations (designer-protocol-v1 §10.1). */
export type PatchDestination = "inline" | "class" | "token";

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
  /**
   * Asks for the patch a commit would produce, and shows the edits or the rejection reason.
   *
   * **It does not write anything.** The viewer produces a `source.patch`; applying it is the host's
   * job (designer-protocol-v1 §10.3), and this page has no host. The panel therefore says "edits
   * produced", never "file written" — claiming otherwise would be a lie about the developer's file.
   */
  commit(destination: PatchDestination): void;
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

/**
 * The commit section: one button per destination, and the answer the last commit produced.
 *
 * **Why the answer is shown as edits and not as "done".** The viewer produces a patch; the host
 * applies it (designer-protocol-v1 §10.3). This page has no host, so the honest report is the edit
 * list — file, byte range, replacement — plus a line saying it was produced and not applied. A
 * button that said "committed" would be claiming a write that never happened.
 */
function commitBlock(
  report: CanvasPatchReport | null,
  onCommit: (destination: PatchDestination) => void,
): HTMLElement {
  const block = document.createElement("div");
  block.className = "inspect-commit";
  const title = document.createElement("div");
  title.className = "inspect-section";
  title.textContent = "commit";
  block.appendChild(title);

  const buttons = document.createElement("div");
  buttons.className = "inspect-commit-actions";
  const destinations: { id: PatchDestination; label: string }[] = [
    { id: "inline", label: "inline" },
    { id: "class", label: "class" },
    { id: "token", label: "token" },
  ];
  for (const destination of destinations) {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "inspect-commit-button";
    button.dataset.destination = destination.id;
    button.textContent = destination.label;
    button.addEventListener("click", () => onCommit(destination.id));
    buttons.appendChild(button);
  }
  block.appendChild(buttons);

  if (report === null) {
    return block;
  }
  const answer = document.createElement("div");
  answer.className = "inspect-commit-answer";
  if (!report.ok) {
    // The spec's reason, verbatim, so a developer can look it up; the detail is the sentence that
    // says which node or file it was about.
    answer.dataset.ok = "false";
    answer.textContent = `no patch: ${report.reason ?? "unknown"}${
      report.detail !== undefined && report.detail !== "" ? ` — ${report.detail}` : ""
    }`;
    block.appendChild(answer);
    return block;
  }
  answer.dataset.ok = "true";
  const edits = report.edits ?? [];
  const versions = report.baseVersions ?? {};
  const files = Object.keys(versions);
  const header = document.createElement("div");
  header.className = "inspect-commit-summary";
  // "produced", not "applied": the host is what writes, and this page is not the host.
  header.textContent = `${edits.length} edit${edits.length === 1 ? "" : "s"} produced in ${
    files.length
  } file${files.length === 1 ? "" : "s"} — not applied (no host)`;
  answer.appendChild(header);
  for (const edit of edits) {
    const row = document.createElement("div");
    row.className = "inspect-edit";
    row.dataset.file = edit.file;
    const where = document.createElement("code");
    where.className = "inspect-edit-range";
    where.textContent = `${edit.file}:${edit.offset}+${edit.length}`;
    const text = document.createElement("code");
    text.className = "inspect-edit-text";
    text.textContent = edit.newText;
    row.replaceChildren(where, text);
    answer.appendChild(row);
  }
  block.appendChild(answer);
  return block;
}

export function createCanvasInspector(options: CanvasInspectorOptions): CanvasInspector {
  const { panel, body, toggle, canvas } = options.elements;
  const setStatus = options.setStatus;
  let isArmed = false;
  // The last node asked about, so a frame that moved the boxes can re-ask without a new click.
  let last: { clientX: number; clientY: number } | null = null;
  // The node the panel is showing, so a commit names the same node the panel describes even after
  // the pointer moved away. Zero means no node is selected.
  let selected = 0;
  // The last commit's answer, kept so a re-render (a frame moved the boxes) does not lose it.
  let lastPatch: CanvasPatchReport | null = null;

  function hide(): void {
    panel.hidden = true;
    last = null;
    selected = 0;
    lastPatch = null;
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
    selected = Number(report.node ?? "0");

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

    // The commit action is last: it is the write half, and a developer reads the style before
    // deciding to commit it.
    sections.push(commitBlock(lastPatch, commit));

    body.replaceChildren(...sections);
    panel.hidden = false;
  }

  function commit(destination: PatchDestination): void {
    const module = options.module();
    if (module === null || selected === 0) {
      return;
    }
    // The declarations a commit writes are the node's overrides (designer-protocol-v1 §10.1). The
    // override layer is `style.override.set`, a later slice, so this slice has none to commit and
    // the engine answers `no_overrides`. The panel shows that reason rather than inventing a value:
    // a commit that guessed a declaration would write something the developer never chose.
    const declarations = "";
    let report: CanvasPatchReport;
    try {
      report = JSON.parse(
        callExport(
          module,
          "_tessera_canvas_patch",
          [String(selected), destination, "", "", "", "", declarations],
          [],
        ),
      ) as CanvasPatchReport;
    } catch (error) {
      setStatus("commit unavailable: " + String(error), "warning");
      return;
    }
    lastPatch = report;
    // Re-render the panel so the answer appears in place. The style report is unchanged, so it is
    // re-asked rather than kept: the panel is a view of the running tree, and a stale copy would
    // show a node that a rebuild may have replaced.
    if (last !== null) {
      ask(last.clientX, last.clientY);
    }
    if (report.ok) {
      const count = (report.edits ?? []).length;
      setStatus(
        `commit produced ${count} edit${count === 1 ? "" : "s"} — not applied (no host)`,
        "ok",
      );
    } else {
      setStatus(`commit rejected: ${report.reason ?? "unknown"}`, "warning");
    }
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
    commit,
  };
}
