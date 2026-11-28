// SPDX-License-Identifier: MIT

// Format Document and format-on-save (#577, ADR-061 slice 2): the editor side of the `format` service.
//
// The page owns the text (studio-services-v1 section 4), so each format sends the editor's current text
// as a snapshot and asks for `format`. The answer is an edit list in the coordinates of that text (UTF-16
// offsets, what CodeMirror counts), applied as ONE transaction: one undo step restores the text, the
// caret and selection map through the changes, and a refused file leaves the text untouched and says why.
// If the person typed while the worker was busy the answer is for old text and is dropped (the client
// rejects it as stale, and the text check below catches a snapshot the client could not see).

import { ChangeSet, type ChangeSpec } from "@codemirror/state";
import { EditorView } from "@codemirror/view";
import { ServiceClient } from "../../services/client.js";
import { formatServiceSource, wasmEngineSource } from "../../services/format-service.js";
import { wasmModuleUrl } from "../lib/module-url";

/** What `format` answers (format-service.js). */
interface FormatAnswer {
  changed: boolean;
  edits: { offset: number; length: number; newText: string }[];
  diagnostics: { code: string; message: string; line: number; column: number }[];
}

const FILE_ID = 0;

let client: ServiceClient | null = null;
let starting: Promise<ServiceClient> | null = null;

function browserWorker(source: string) {
  const url = URL.createObjectURL(new Blob([source], { type: "text/javascript" }));
  const worker = new Worker(url);
  return {
    post: (message: object, transfer: ArrayBuffer[] = []) => worker.postMessage(message, transfer),
    onMessage: (listener: (message: unknown) => void) => worker.addEventListener("message", (event) => listener(event.data)),
    onError: (listener: (message: string) => void) => worker.addEventListener("error", (event) => listener(event.message || "worker error")),
    terminate: () => {
      worker.terminate();
      URL.revokeObjectURL(url);
    },
  };
}

/** The format worker, started on first use (the Wasm module loads on the first format, not at startup). */
function formatClient(): Promise<ServiceClient> {
  starting ??= (async () => {
    const created = new ServiceClient({
      name: "format",
      createWorker: () => browserWorker(formatServiceSource(wasmEngineSource(wasmModuleUrl("tessera_format.js")))),
    });
    await created.start();
    client = created;
    return created;
  })().catch((error) => {
    starting = null;
    throw error;
  });
  return starting;
}

/** The outcome shown to the person: formatted, already canonical, or the file the formatter refused. */
export type FormatOutcome = "formatted" | "unchanged" | "refused" | "skipped";

export interface FormatReport {
  outcome: FormatOutcome;
  /** The refusal, as `line:column code message`; empty otherwise. */
  message: string;
}

/** Whether the studio formats a file of this path (`.tsx` only until CSS and tessera.json arrive, slice 3). */
export function isFormattable(path: string): boolean {
  return path.endsWith(".tsx");
}

/**
 * Formats the text `view` shows. Resolves with the outcome; rejects only when the worker fails.
 * Never touches the text when the file is refused, when it is already canonical, or when it changed while
 * the worker was busy.
 */
export async function formatView(view: EditorView): Promise<FormatReport> {
  const path = view.contentDOM.dataset.path ?? "";
  if (!isFormattable(path) || view.state.readOnly) {
    return { outcome: "skipped", message: "" };
  }
  const text = view.state.doc.toString();
  const service = await formatClient();
  await service.setSnapshot([[FILE_ID, text]]);
  let answer: FormatAnswer;
  try {
    answer = (await service.request("format", { file: FILE_ID })) as FormatAnswer;
  } catch (error) {
    if ((error as { code?: string }).code === "TESSERA_STUDIO_STALE" || (error as { code?: string }).code === "TESSERA_STUDIO_CANCELLED") {
      return { outcome: "skipped", message: "" };
    }
    throw error;
  }
  if (view.state.doc.toString() !== text) {
    return { outcome: "skipped", message: "" };
  }
  if (answer.diagnostics.length > 0) {
    const first = answer.diagnostics[0];
    return { outcome: "refused", message: `${path}:${first.line}:${first.column} ${first.code}: ${first.message}` };
  }
  if (!answer.changed) {
    return { outcome: "unchanged", message: "" };
  }
  const changes: ChangeSpec[] = answer.edits.map((edit) => ({ from: edit.offset, to: edit.offset + edit.length, insert: edit.newText }));
  // Validates the list against the document before dispatching: a bad range throws here, not mid-transaction.
  ChangeSet.of(changes, text.length);
  view.dispatch({ changes, userEvent: "format", scrollIntoView: true });
  return { outcome: "formatted", message: "" };
}

/** Releases the worker (tests and teardown). */
export function disposeFormatWorker(): void {
  client?.dispose();
  client = null;
  starting = null;
}
