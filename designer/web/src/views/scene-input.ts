// SPDX-License-Identifier: MIT

// The input-script half of the Scene view: the `click <automationId>` script, the "Send clicks" run, and
// the state-cell table it fills (issue #168, P4).
//
// It is its own module because the Scene view also carries the snapshot: the two halves share only the
// page's one text-call helper, and keeping both together pushed the file past the soft source limit. The
// seam is deliberately narrow -- this module never touches the Wasm heap, the module factory or
// `tessera_free`. It asks `request` for the document text, so the session remains the only owner of
// every allocation and of its matching free.
//
// The document is the one `tools/codegen/scene_interaction.hpp` defines, and
// `scripts/ci/scene_input_parity.py` is what keeps the shape honest:
//
//   {"inputVersion":1,
//    "initial":[{"name":"state::count","kind":"i32","value":{"value":"0"}}, ...],
//    "steps":[{"step":0,"kind":"click","automationId":"101","found":true,"dispatched":true,
//              "ran":["handler::1"],"unrun":[],"state":[...]}, ...]}
//
// or `{"error":"..."}` when the script or the source produced nothing to click.

import type { SourceReader, StatusReporter } from "../lib/views";
import { element, unavailableBox } from "./dom";

/** One state cell, as the document spells it. */
interface Cell {
  name?: string;
  value?: { value?: unknown } | unknown[] | null;
}

/** One step of the script, with the state cells it left behind. */
interface Step {
  automationId?: string;
  found?: boolean;
  dispatched?: boolean;
  state?: Cell[];
  ran?: string[];
  unrun?: string[];
}

export interface InteractionDocument {
  initial?: Cell[];
  steps?: Step[];
  error?: string;
}

/** The elements the input pane needs; all of them are React refs. */
export interface InputPaneElements {
  script: HTMLTextAreaElement;
  cells: HTMLElement;
  button: HTMLButtonElement | null;
}

/** What the session lends this pane: the document request and the pane's status line. */
export interface InputPaneOptions {
  elements: InputPaneElements;
  /**
   * What Run compiles (issue #431): the workspace's compile target. The click script is applied to the
   * same source the node list was built from, so the two cannot describe different documents.
   */
  source: SourceReader;
  /** Asks the session for the module's document text, so the heap stays the session's. */
  request: (name: string, args: string[]) => Promise<string>;
  setStatus: StatusReporter;
  /** Reports readiness to the page's shared inputs (the "Send clicks" button). */
  setEnabled: (enabled: boolean) => void;
}

export interface InputPane {
  run(): Promise<void>;
  /** Appends `click <id>` to the script (a node row's picker). */
  append(automationId: string): void;
  /** Clears the cells: they describe a document this one no longer is. */
  reset(): void;
}

// One state cell as a readable row: the cell id, then its value. A list cell keeps its brackets so a
// snapshot read by eye matches the document's own shape.
function describeCell(cell: Cell): string {
  const value = cell.value;
  if (value === undefined || value === null) {
    return "(no value)";
  }
  if (Array.isArray(value)) {
    return "[" + value.map((item) => String((item as { value?: unknown }).value ?? "")).join(", ") + "]";
  }
  return String((value as { value?: unknown }).value ?? "");
}

function renderCells(cells: HTMLElement, setStatus: StatusReporter, snapshot: InteractionDocument): void {
  cells.replaceChildren();
  const initial = Array.isArray(snapshot.initial) ? snapshot.initial : [];
  const steps = Array.isArray(snapshot.steps) ? snapshot.steps : [];
  if (initial.length === 0 && steps.length === 0) {
    setStatus("no state cells", "warning");
    return;
  }
  // The initial state first, then every step's cells, so the sequence is visible rather than only the
  // final value: a preview that dropped intermediate state still ends at the same place in a simple
  // counter, and this is what makes that difference readable.
  const addRow = (label: string, state: Cell[], extra: boolean): void => {
    const row = element("li", extra ? "cell step" : "cell");
    row.append(element("span", "cell-when", label));
    if (state.length === 0) {
      row.append(element("span", "cell-name", "(no cells)"));
    }
    for (const cell of state) {
      const pair = element("span", "cell-pair");
      pair.append(
        element("span", "cell-name", String(cell.name ?? "")),
        element("span", "cell-value", describeCell(cell)),
      );
      row.append(pair);
    }
    cells.append(row);
  };
  addRow("initial", initial, false);
  for (const step of steps) {
    const label = `click ${step.automationId}`;
    const note = step.found ? (step.dispatched ? "" : " (not dispatched)") : " (no such node)";
    addRow(label + note, Array.isArray(step.state) ? step.state : [], true);
  }
  const last = steps.length > 0 ? steps[steps.length - 1] : null;
  if (last === null) {
    setStatus(`${initial.length} cell(s) at their initial value`, "ok");
    return;
  }
  const ran = (last.ran ?? []).length;
  const unrun = (last.unrun ?? []).length;
  const parts = [`${steps.length} step(s)`, `${ran} handler run`];
  if (unrun > 0) {
    parts.push(`${unrun} handler not run`);
  }
  setStatus(parts.join(", "), unrun > 0 ? "warning" : "ok");
}

/** Wires the input pane's behaviour to the elements React rendered. */
export function createInputPane(options: InputPaneOptions): InputPane {
  const { script, cells, button } = options.elements;
  const setStatus = options.setStatus;

  function showProblem(heading: string, detail: string): void {
    cells.replaceChildren();
    cells.append(unavailableBox(heading, detail));
    setStatus("failed", "warning");
  }

  async function run(): Promise<void> {
    if (button) {
      button.disabled = true;
    }
    setStatus("clicking", null);
    try {
      const target = options.source();
      const json = await options.request("_tessera_input_json", [
        target.text,
        target.file,
        "",
        script.value,
      ]);
      const snapshot = JSON.parse(json) as InteractionDocument;
      if (snapshot.error) {
        // A malformed script, or a source that produced no scene: reported as such, never as a
        // successful run with no clicks.
        showProblem("No input result", String(snapshot.error));
        return;
      }
      renderCells(cells, setStatus, snapshot);
    } catch (error) {
      showProblem("Input failed", String(error));
    } finally {
      if (button) {
        button.disabled = false;
      }
    }
  }

  // Appending a node's id to the script is what makes the page usable without a hash table in the
  // reader's head: the ids are 64-bit numbers, so nobody types them.
  function append(automationId: string): void {
    if (!automationId || automationId === "0") {
      return;
    }
    const line = `click ${automationId}`;
    const current = script.value.trim();
    script.value = current ? `${current}\n${line}\n` : `${line}\n`;
    setStatus(`added ${line}`, "ok");
  }

  // Called when the page loads a different source: the cells on screen describe the document that was
  // evaluated, so leaving them up would show another sample's state beside this one's node list.
  function reset(): void {
    cells.replaceChildren();
    setStatus("no clicks yet", null);
  }

  button?.addEventListener("click", () => {
    void run();
  });

  return { run, append, reset };
}
