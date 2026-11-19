// SPDX-License-Identifier: MIT

// The page-side protocol client of the style override layer (issue #585, designer-protocol-v1 sections 9
// and 10).
//
// **What it is.** The canvas module owns the override layer (`tessera_canvas_override`), the settle that
// closes a commit (`tessera_canvas_settle`) and the patch a commit produces (`tessera_canvas_patch`). This
// client is the page's half of those messages: it sets, clears, reverts, undoes and redoes overrides, and it
// runs the commit round trip of section 10.3 (`style.commit` -> `source.patch` -> apply to the file buffer ->
// `source.patch.result` -> `source.update` evaluated -> settle).
//
// **What it is not.** It touches no DOM and no React, and it never calls a module itself: it is handed
// `invoke(exportName, strings)` (the page binds `callExport`) and a `CommitHost` (the file buffers, the editor
// and the evaluation). So the Node gate drives it against a fake module and fake buffers, and the studio
// rebuilt with Tessera can supply its own host.
//
// **The one rule the commit enforces for the text history (D3, issue #585).** A commit writes ONE entry per
// edited file (`CommitHost.applyEdits` is one editor transaction), and a rejected patch writes none: the edit
// is only applied after every base version matched and every edit range converted.

import type { ThemeHandover } from "./wasm-call";
import { applyChanges, EditRangeError, toTextChanges, type TextChange } from "./utf8-edits.ts";

/** `callExport` bound to a module: strings in, the module's JSON document out; throws when unavailable. */
export type Invoke = (name: string, strings: string[]) => string;

export interface OverrideEntry {
  /** Decimal string, like every id the module reports. */
  node: string;
  property: string;
  value: string;
}

export interface OverrideDiagnostic {
  /** `TESSERA_CSS_*`, `TESSERA_DESIGNER_LIMIT` or `TESSERA_DESIGNER_PATCH_MISMATCH`. */
  code: string;
  message: string;
}

/** What one override operation answered (`canvas_override_report_json`). */
export interface OverrideResult {
  ok: boolean;
  /** `no_such_node`, `unknown_operation`, `invalid_override`, or `unavailable` (this build lacks the export). */
  reason?: string;
  /** True when the layer changed and a frame is owed. */
  changed: boolean;
  canUndo: boolean;
  canRedo: boolean;
  diagnostics: OverrideDiagnostic[];
  overrides: OverrideEntry[];
}

/** The override stack as the page shows it: what Undo and Redo would do, and their menu words. */
export interface OverrideState {
  canUndo: boolean;
  canRedo: boolean;
  /** What Undo would undo, e.g. `Set color on Button`; `""` when the stack is empty. */
  undoLabel: string;
  redoLabel: string;
  /** Every override in the layer, as the last report listed them. */
  overrides: OverrideEntry[];
}

export type CommitDestination = "inline" | "class" | "token";

/** One `style.commit` (section 10.1). */
export interface CommitRequest {
  node: string;
  /** For the history label (`Commit style to Button`); never sent to the module. */
  nodeLabel: string;
  destination: CommitDestination;
  className?: string;
  /** The `class` destination's CSS file. */
  file?: string;
  /** The `token` destination's custom property, with its `--`, and the value it takes. */
  token?: string;
  tokenValue?: string;
}

/** What the commit round trip ended as. */
export type CommitOutcome =
  /** The module could not turn the commit into edits (`style.commit.result` `ok:false`). */
  | { status: "no-patch"; reason: string; detail: string }
  /** The host refused the patch (`source.patch.result` `rejected`); overrides and both histories untouched. */
  | { status: "rejected"; reason: string; detail: string }
  /** Applied, evaluated and settled: the overrides became the source. */
  | { status: "committed"; edits: number; files: string[] }
  /** Applied, but the new source did not give the node the overridden style: overrides kept. */
  | { status: "mismatch"; edits: number; files: string[]; diagnostics: OverrideDiagnostic[] }
  /** Applied, but the source did not evaluate; the settle waits for the next successful evaluation. */
  | { status: "unsettled"; edits: number; files: string[] }
  /** A patch for this node is already open (section 10.3: no second `source.patch`). */
  | { status: "busy" };

/** What the commit needs from the page: the buffers, the editor and the evaluation. */
export interface CommitHost {
  text(path: string): string;
  /**
   * True when `path`'s buffer is exactly the text the module has staged (and so the patch's offsets).
   *
   * This is the page's reading of `baseVersions`: the module numbers every staged file 1, so the version
   * that can differ is the buffer's. A keystroke inside the live-edit debounce makes it false.
   */
  isCurrent(path: string): boolean;
  /** Applies `changes` (UTF-16, against the current text) to `path` as ONE history entry. */
  applyEdits(path: string, changes: TextChange[], label: string): void;
  /**
   * Evaluates the new source (the `source.update` of section 10.3) and resolves `true` when it
   * evaluated, `false` when it did not compile. Settle is only meaningful after it.
   */
  evaluate(): Promise<boolean>;
  /** The theme asset as the buffers hold it (for the `token` destination), or `null`. */
  theme(): ThemeHandover | null;
}

/**
 * What the page gives a canvas session so it can build a `CommitHost`: the buffers, the editor and the
 * live-edit trigger. The session adds what only it knows (which text the module has staged, when an
 * evaluation finished).
 */
export interface CommitBridge {
  text(path: string): string;
  applyEdits(path: string, changes: TextChange[], label: string): void;
  /** Evaluates the buffers now (`Ctrl+Enter`'s path), skipping the debounce. */
  applyNow(): Promise<void>;
}

export interface OverrideClient {
  set(node: string, property: string, value: string, label?: string): OverrideResult;
  clear(node: string, property?: string, label?: string): OverrideResult;
  revert(): OverrideResult;
  undo(): OverrideResult;
  redo(): OverrideResult;
  state(): OverrideState;
  /** The node whose commit is waiting for an evaluation to settle, or `null`. */
  pendingSettle(): string | null;
  /** The page's evaluation hook: call after every successful evaluation; settles a waiting commit. */
  evaluated(): CommitOutcome | null;
  commit(request: CommitRequest, host: CommitHost): Promise<CommitOutcome>;
  /** The session ended (Restart app, a new project): overrides and their history are gone. */
  reset(): void;
  subscribe(listener: () => void): () => void;
}

interface HistoryLabel {
  label: string;
  /** The node the operation touched, or `""` for a revert (every node). */
  node: string;
}

function emptyResult(reason: string, message: string): OverrideResult {
  return {
    ok: false,
    reason,
    changed: false,
    canUndo: false,
    canRedo: false,
    diagnostics: [{ code: "TESSERA_DESIGNER_UNAVAILABLE", message }],
    overrides: [],
  };
}

function parseOverride(text: string): OverrideResult {
  const raw = JSON.parse(text) as Partial<OverrideResult>;
  return {
    ok: raw.ok === true,
    reason: raw.reason,
    changed: raw.changed === true,
    canUndo: raw.canUndo === true,
    canRedo: raw.canRedo === true,
    diagnostics: raw.diagnostics ?? [],
    overrides: raw.overrides ?? [],
  };
}

interface SettleReport {
  ok?: boolean;
  reason?: string;
  consumed?: boolean;
  diagnostics?: OverrideDiagnostic[];
  overrides?: OverrideEntry[];
  canUndo?: boolean;
  canRedo?: boolean;
}

interface PatchReport {
  ok?: boolean;
  reason?: string;
  detail?: string;
  patchId?: string;
  edits?: { file: string; offset: number; length: number; newText: string }[];
  baseVersions?: Record<string, number>;
}

export function createOverrideClient(options: { invoke: Invoke }): OverrideClient {
  const { invoke } = options;
  const listeners = new Set<() => void>();
  // Mirrors the module's snapshot history with the words a menu needs. The module's `canUndo`/`canRedo`
  // are authoritative for enabled-ness; these stacks only supply the label of the top entry.
  let undoStack: HistoryLabel[] = [];
  let redoStack: HistoryLabel[] = [];
  let canUndo = false;
  let canRedo = false;
  let overrides: OverrideEntry[] = [];
  const open = new Set<string>();
  let pending: { node: string; outcome: Omit<Extract<CommitOutcome, { status: "unsettled" }>, "status"> } | null = null;

  const notify = (): void => {
    for (const listener of [...listeners]) {
      listener();
    }
  };

  function run(operation: string, node: string, property: string, value: string): OverrideResult {
    try {
      return parseOverride(invoke("_tessera_canvas_override", [operation, node, property, value]));
    } catch (error) {
      return emptyResult("unavailable", String(error));
    }
  }

  // `set`/`clear` answer only the named node's overrides; `revert`/`undo`/`redo` answer every node's.
  function absorb(result: OverrideResult, node: string | null): void {
    canUndo = result.canUndo;
    canRedo = result.canRedo;
    overrides =
      node === null
        ? result.overrides
        : [...overrides.filter((entry) => entry.node !== node), ...result.overrides];
    if (!canUndo) {
      undoStack = [];
    }
    if (!canRedo) {
      redoStack = [];
    }
  }

  function operation(kind: "set" | "clear" | "revert", node: string, property: string, value: string, label: string): OverrideResult {
    const result = run(kind, node, property, value);
    if (result.ok && result.changed) {
      undoStack.push({ label, node: kind === "revert" ? "" : node });
      redoStack = [];
    }
    if (result.ok) {
      absorb(result, kind === "revert" ? null : node);
      notify();
    }
    return result;
  }

  function history(kind: "undo" | "redo"): OverrideResult {
    const result = run(kind, "", "", "");
    if (result.ok && result.changed) {
      const from = kind === "undo" ? undoStack : redoStack;
      const to = kind === "undo" ? redoStack : undoStack;
      const entry = from.pop();
      if (entry !== undefined) {
        to.push(entry);
      }
    }
    if (result.ok) {
      absorb(result, null);
      notify();
    }
    return result;
  }

  function settle(node: string): { report: SettleReport | null; error: string } {
    try {
      return { report: JSON.parse(invoke("_tessera_canvas_settle", [node])) as SettleReport, error: "" };
    } catch (error) {
      return { report: null, error: String(error) };
    }
  }

  // After a settle consumed `node`, the module dropped that node's operations from its history (and kept the
  // others'). The labels follow by node; the module's own can-flags decide whether an entry exists at all.
  function consumed(node: string, report: SettleReport): void {
    undoStack = undoStack.filter((entry) => entry.node !== node);
    redoStack = redoStack.filter((entry) => entry.node !== node);
    overrides = overrides.filter((entry) => entry.node !== node);
    if (typeof report.canUndo === "boolean") {
      canUndo = report.canUndo;
      canRedo = report.canRedo === true;
    } else {
      // An older module does not report the flags after a settle: what is left is what the labels say.
      canUndo = canUndo && undoStack.length > 0;
      canRedo = canRedo && redoStack.length > 0;
    }
    if (!canUndo) {
      undoStack = [];
    }
    if (!canRedo) {
      redoStack = [];
    }
  }

  function finishSettle(
    node: string,
    applied: { edits: number; files: string[] },
  ): CommitOutcome {
    const { report, error } = settle(node);
    if (report === null) {
      return { status: "mismatch", ...applied, diagnostics: [{ code: "TESSERA_DESIGNER_UNAVAILABLE", message: error }] };
    }
    if (report.ok === true || report.reason === "no_such_node") {
      // `no_such_node` after a commit means the evaluation replaced the tree (a restart); the overrides went
      // with the session, which is the settled outcome.
      if (report.consumed === true) {
        consumed(node, report);
      } else {
        overrides = overrides.filter((entry) => entry.node !== node);
      }
      notify();
      return { status: "committed", ...applied };
    }
    if (report.overrides !== undefined) {
      overrides = [...overrides.filter((entry) => entry.node !== node), ...report.overrides];
    }
    notify();
    return { status: "mismatch", ...applied, diagnostics: report.diagnostics ?? [] };
  }

  async function commit(request: CommitRequest, host: CommitHost): Promise<CommitOutcome> {
    if (open.has(request.node) || pending?.node === request.node) {
      return { status: "busy" };
    }
    open.add(request.node);
    try {
      if (request.destination === "token") {
        // The module only knows the theme asset if the page hands it over, and the patch's offsets are
        // measured in the text it was handed, so it is the buffer as the editor holds it right now.
        const theme = host.theme();
        try {
          invoke("_tessera_canvas_set_theme", theme === null ? ["", "", ""] : [theme.path, theme.text, theme.themeId]);
        } catch {
          // A module built without the export keeps its `not_token_bound` answer, which is the honest one.
        }
      }
      let report: PatchReport;
      try {
        report = JSON.parse(
          invoke("_tessera_canvas_patch", [
            request.node,
            request.destination,
            request.className ?? "",
            request.file ?? "",
            request.token ?? "",
            request.tokenValue ?? "",
          ]),
        ) as PatchReport;
      } catch (error) {
        return { status: "no-patch", reason: "unavailable", detail: String(error) };
      }
      if (report.ok !== true) {
        return { status: "no-patch", reason: report.reason ?? "unknown", detail: report.detail ?? "" };
      }
      const edits = report.edits ?? [];
      const files = Object.keys(report.baseVersions ?? {});
      // Section 10.3: compare every base version before applying anything (the patch is atomic).
      for (const path of files) {
        if (!host.isCurrent(path)) {
          return {
            status: "rejected",
            reason: "version_mismatch",
            detail: `${path} changed since the preview last evaluated it; commit again after the preview catches up`,
          };
        }
      }
      // Convert and validate every file before touching any buffer, for the same atomicity.
      const planned: { path: string; changes: TextChange[] }[] = [];
      try {
        for (const path of files) {
          const text = host.text(path);
          const changes = toTextChanges(text, edits.filter((edit) => edit.file === path));
          applyChanges(text, changes);
          planned.push({ path, changes });
        }
      } catch (error) {
        if (error instanceof EditRangeError) {
          return { status: "rejected", reason: "edit_out_of_range", detail: error.message };
        }
        throw error;
      }
      const label = `Commit style to ${request.nodeLabel}`;
      for (const { path, changes } of planned) {
        host.applyEdits(path, changes, label);
      }
      const applied = { edits: edits.length, files };
      // `source.patch.result` applied, then the `source.update`: the host evaluates the new text, and only
      // then is settle meaningful.
      const evaluated = await host.evaluate();
      if (!evaluated) {
        pending = { node: request.node, outcome: applied };
        notify();
        return { status: "unsettled", ...applied };
      }
      return finishSettle(request.node, applied);
    } finally {
      open.delete(request.node);
    }
  }

  return {
    set: (node, property, value, label) =>
      operation("set", node, property, value, label ?? `Set ${property}`),
    clear: (node, property, label) =>
      operation("clear", node, property ?? "", "", label ?? (property ? `Clear ${property}` : "Clear overrides")),
    revert: () => operation("revert", "", "", "", "Revert all overrides"),
    undo: () => history("undo"),
    redo: () => history("redo"),
    state: () => ({
      canUndo,
      canRedo,
      undoLabel: canUndo ? (undoStack.at(-1)?.label ?? "style change") : "",
      redoLabel: canRedo ? (redoStack.at(-1)?.label ?? "style change") : "",
      overrides,
    }),
    pendingSettle: () => pending?.node ?? null,
    evaluated: () => {
      if (pending === null) {
        return null;
      }
      const { node, outcome } = pending;
      pending = null;
      return finishSettle(node, outcome);
    },
    commit,
    reset: () => {
      undoStack = [];
      redoStack = [];
      canUndo = false;
      canRedo = false;
      overrides = [];
      pending = null;
      open.clear();
      notify();
    },
    subscribe: (listener) => {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
  };
}
