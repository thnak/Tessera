// SPDX-License-Identifier: MIT

// The editor's code intelligence (#984, #988): F12, Shift+F12, hover, completion and rename, all answered by the
// `analysis` service (studio-services-v1 sections 12 and 13). The page owns the text, so each request sends the
// project (the open file taken from the editor) and asks at a UTF-16 offset; an unchanged project is not resent.
//
// **One hub, attached by the page.** The commands and the CodeMirror extensions are created before the
// workspace exists, so they talk to this object and the page attaches the project (files, text, how to open a
// span, how to write buffers) once it has one. Without a project, or without the Wasm module on the site, every
// feature answers quietly with nothing and `ready()` is false, so the `go.*` commands disable with a reason.
//
// **Rename is one undo step across files.** The plan is applied to the workspace buffers (the page's one store of
// unsaved text) and recorded as ONE entry in a replace history; `undoRename` restores every file that still holds
// the renamed text and says which it left alone because it changed since. CodeMirror's own per-file history
// cannot span files, which is why the entry lives here.
//
// No DOM: a Node gate drives this module as the page ships it.

import { createAnalysisClient, type AnalysisClient } from "../lib/analysis-client";
import {
  completionOptions,
  definitionResult,
  hoverCard,
  idOfPath,
  navEntries,
  referenceRows,
  renameLabel,
  renamePlan,
  type CompletionOption,
  type HoverCard,
  type NavProject,
  type ReferenceRow,
  type Target,
} from "../lib/analysis-nav";
import { createReplaceHistory, type ReplaceHistory } from "../lib/search-replace";

export interface HubNotice {
  message: string;
  severity?: "info" | "warning" | "error";
  key?: string;
  actions?: { label: string; command: string }[];
}

/** What the page lends the hub once it has a project. */
export interface HubProject extends NavProject {
  files: readonly { path: string; kind: string; editable?: boolean }[];
  /** Shows the span in the editor, opening its file first. */
  openAt: (target: Target) => void;
  /** Writes the new texts into the workspace buffers (files turn dirty, open tabs reload). */
  writeBuffers: (writes: readonly { path: string; text: string }[]) => void;
  notify: (notice: HubNotice) => void;
  /** Offers several references to choose from (a quick pick). */
  pickReference: (rows: readonly ReferenceRow[]) => void;
}

/** The caret in the open file: its path, the editor's text and the UTF-16 offset. */
export interface Position {
  path: string;
  text: string;
  offset: number;
}

export interface AnalysisHub {
  attach(project: HubProject | null): () => void;
  /** False once the worker proved unavailable. */
  ready(): boolean;
  subscribe(listener: () => void): () => void;
  definition(at: Position): Promise<void>;
  references(at: Position): Promise<void>;
  rename(at: Position): Promise<void>;
  undoRename(): void;
  hover(at: Position): Promise<HoverCard | null>;
  complete(at: Position): Promise<{ from: number; to: number; options: CompletionOption[]; context: string } | null>;
  canUndoRename(): boolean;
  dispose(): void;
}

export interface HubOptions {
  createClient?: () => AnalysisClient;
  /** The new name for a rename; `null` cancels. The page's prompt by default. */
  askName?: (current: string) => string | null;
}

const defaultAsk = (current: string): string | null => globalThis.prompt?.(`Rename ${current} to:`, current) ?? null;

export function createAnalysisHub(options: HubOptions = {}): AnalysisHub {
  let project: HubProject | null = null;
  let client: AnalysisClient | null = null;
  let wasReady = true;
  const history: ReplaceHistory = createReplaceHistory();
  const listeners = new Set<() => void>();

  const service = (): AnalysisClient => (client ??= (options.createClient ?? createAnalysisClient)());
  const changed = (): void => {
    const now = client?.available() ?? true;
    if (now !== wasReady) {
      wasReady = now;
      for (const listener of [...listeners]) listener();
    }
  };

  /** One request at the caret; `null` with a notice when there is nothing to ask or the worker said no. */
  async function ask(at: Position, type: string, extra: Record<string, unknown>, quiet: boolean): Promise<{ answer: unknown; entries: ReturnType<typeof navEntries> } | null> {
    const current = project;
    if (current === null) {
      return null;
    }
    const entries = navEntries(current, at);
    const file = idOfPath(entries, at.path);
    if (file < 0) {
      if (!quiet) current.notify({ message: "Code intelligence works in the project's .tsx, .ts and .css files.", severity: "info", key: "analysis-kind" });
      return null;
    }
    const outcome = await service().query(entries, type, { file, offset: at.offset, ...extra });
    changed();
    if (outcome === null) {
      return null;
    }
    if (!outcome.ok) {
      if (!quiet) current.notify({ message: outcome.message, severity: "warning", key: "analysis-unavailable" });
      return null;
    }
    return { answer: outcome.value, entries };
  }

  return {
    attach(next) {
      project = next;
      return () => {
        if (project === next) project = null;
      };
    },
    ready: () => wasReady,
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    async definition(at) {
      const got = await ask(at, "definition", {}, false);
      if (got === null) return;
      const { target, message } = definitionResult(got.answer, got.entries);
      if (target === null) {
        project?.notify({ message, severity: "info", key: "analysis-definition" });
      } else {
        project?.openAt(target);
      }
    },
    async references(at) {
      const got = await ask(at, "references", { includeDeclaration: true }, false);
      if (got === null) return;
      const rows = referenceRows(got.answer, got.entries);
      if (rows.length === 0) {
        project?.notify({ message: "No references found at the caret.", severity: "info", key: "analysis-references" });
      } else if (rows.length === 1) {
        project?.openAt(rows[0]);
      } else {
        project?.pickReference(rows);
      }
    },
    async rename(at) {
      const prepared = await ask(at, "prepareRename", {}, false);
      const current = project;
      if (prepared === null || current === null) return;
      const prepare = (prepared.answer as { prepare: { placeholder: string } | null; refusal?: { message: string } });
      if (prepare.prepare === null) {
        current.notify({ message: prepare.refusal?.message ?? "This name cannot be renamed.", severity: "info", key: "analysis-rename" });
        return;
      }
      const from = prepare.prepare.placeholder;
      const name = (options.askName ?? defaultAsk)(from)?.trim();
      if (name === undefined || name === "" || name === from) return;
      const got = await ask(at, "rename", { newName: name }, false);
      if (got === null) return;
      const plan = renamePlan(got.answer, got.entries);
      if (!plan.ok) {
        current.notify({ message: plan.message, severity: "warning", key: "analysis-rename" });
        return;
      }
      const locked = plan.changes.find((change) => current.files.find((file) => file.path === change.path)?.editable === false);
      if (locked !== undefined) {
        current.notify({ message: `${locked.path} is locked, so the rename was not applied.`, severity: "warning", key: "analysis-rename" });
        return;
      }
      // The answer is for the texts it was asked about; a file that moved on since would be edited at stale offsets.
      const stale = plan.changes.find((change) => change.path !== at.path && current.textOf(change.path) !== change.before);
      if (stale !== undefined) {
        current.notify({ message: `${stale.path} changed while renaming, so nothing was renamed. Try again.`, severity: "warning", key: "analysis-rename" });
        return;
      }
      const label = renameLabel(from, name, plan.edits, plan.changes.length);
      history.push({ label, changes: plan.changes });
      current.writeBuffers(plan.changes.map((change) => ({ path: change.path, text: change.after })));
      current.notify({ message: `${label}.`, severity: "info", key: "analysis-rename", actions: [{ label: "Undo rename", command: "edit.undoRename" }] });
    },
    undoRename() {
      const current = project;
      const done = current === null ? null : history.undo(current.textOf);
      if (current === null || done === null) return;
      current.writeBuffers(done.writes);
      const left = done.skipped.length === 0 ? "" : ` Left alone because they changed since: ${done.skipped.join(", ")}.`;
      current.notify({ message: `Undid ${done.label}.${left}`, severity: done.skipped.length === 0 ? "info" : "warning", key: "analysis-rename" });
    },
    canUndoRename: () => history.canUndo(),
    async hover(at) {
      const got = await ask(at, "hover", {}, true);
      return got === null ? null : hoverCard(got.answer);
    },
    async complete(at) {
      const got = await ask(at, "completion", {}, true);
      return got === null ? null : completionOptions(got.answer);
    },
    dispose() {
      client?.dispose();
      client = null;
      listeners.clear();
    },
  };
}

/** The page's one hub: the editor extensions and the commands share it. */
export const analysisHub: AnalysisHub = createAnalysisHub();
