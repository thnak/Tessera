// SPDX-License-Identifier: MIT

// The page's side of project search and replace (#580): the query state, the debounced search against the
// `search` worker, click-to-reveal, and replace as ONE multi-file operation with its own undo.
//
// **Why the panel owns a replace history.** The editor's history is per file and holds typing; a replace that
// spans files must undo as a unit, and a file that was never opened has no editor history at all. So a replace
// writes the new texts into the workspace buffers (the page's one store of unsaved text: the files turn dirty,
// open tabs reload) and records the before and after of every file; Undo restores the files that still hold
// the replaced text and says which it skipped because they changed since (`lib/search-replace.ts`).
//
// **Only editable files are searched.** A locked file (generated C++, a font) is never written by the studio, so
// listing it as a replace target would promise something Save cannot keep.

import { useCallback, useEffect, useRef, useState } from "react";
import type { Problem } from "./lib/problems";
import {
  createSearchClient,
  type SearchClient,
  type SearchMatch,
  type SearchQuery,
  type SearchResult,
} from "./lib/search-client";
import {
  applyOriginalEdits,
  createReplaceHistory,
  replaceLabel,
  type FileChange,
  type ReplaceHistory,
  type TextEdit,
} from "./lib/search-replace";
import type { Workspace } from "./page-workspace";

export const DEFAULT_QUERY: SearchQuery = { query: "", caseSensitive: false, wholeWord: false, regex: false, include: "", exclude: "" };

/** How long typing must pause before a search runs; short enough to feel live, long enough to skip keystrokes. */
export const SEARCH_DEBOUNCE_MS = 200;

export type SearchStatus = "idle" | "searching" | "done" | "error";

export interface SearchHandle {
  query: SearchQuery;
  replacement: string;
  showReplace: boolean;
  showFilters: boolean;
  status: SearchStatus;
  result: SearchResult | null;
  /** The pattern error or a refusal, as a sentence; empty when there is none. */
  message: string;
  /** The outcome of the last replace, undo or refusal; shown in the live region until the query changes. */
  notice: string;
  /** Counts completed searches, so a test or a script can wait for the answer to the query it just typed. */
  answered: number;
  collapsed: ReadonlySet<string>;
  canUndo: boolean;
  canRedo: boolean;
  undoLabel: string;
  redoLabel: string;
  setQuery(patch: Partial<SearchQuery>): void;
  setReplacement(text: string): void;
  setShowReplace(show: boolean): void;
  setShowFilters(show: boolean): void;
  toggleFile(path: string): void;
  /** Runs the search again now (also after the panel gains focus, when files may have changed). */
  refresh(): void;
  open(path: string, match: SearchMatch): void;
  replaceMatch(path: string, match: SearchMatch): void;
  replaceFile(path: string): void;
  replaceAll(): void;
  undo(): void;
  redo(): void;
}

/** What the page lends the hook: the workspace, the Problems-style reveal, and a hook for after buffers change. */
export interface SearchDeps {
  workspace: Workspace;
  /** Opens the file and selects the span (the same jump the Problems panel uses). */
  reveal: (problem: Problem) => void;
  /** Called with the paths whose buffers a replace or an undo wrote, so the page reloads its editor and recompiles. */
  onWritten: (paths: readonly string[]) => void;
  /** Whether the panel is the shown tab; a hidden panel runs no search and holds no timer. */
  visible: boolean;
  createClient?: () => SearchClient;
}

export function useSearch(deps: SearchDeps): SearchHandle {
  const [query, setQueryState] = useState<SearchQuery>(DEFAULT_QUERY);
  const [replacement, setReplacement] = useState("");
  const [showReplace, setShowReplace] = useState(false);
  const [showFilters, setShowFilters] = useState(false);
  const [status, setStatus] = useState<SearchStatus>("idle");
  const [result, setResult] = useState<SearchResult | null>(null);
  const [message, setMessage] = useState("");
  const [notice, setNotice] = useState("");
  const [answered, setAnswered] = useState(0);
  const [collapsed, setCollapsed] = useState<ReadonlySet<string>>(() => new Set());
  const [, setTick] = useState(0);

  const depsRef = useRef(deps);
  depsRef.current = deps;
  const clientRef = useRef<SearchClient | null>(null);
  const historyRef = useRef<ReplaceHistory | null>(null);
  historyRef.current ??= createReplaceHistory();
  const history = historyRef.current;
  const token = useRef(0);
  const queryRef = useRef(query);
  queryRef.current = query;
  const replacementRef = useRef(replacement);
  replacementRef.current = replacement;
  const resultRef = useRef(result);
  resultRef.current = result;

  const client = (): SearchClient => {
    clientRef.current ??= (depsRef.current.createClient ?? createSearchClient)();
    return clientRef.current;
  };
  useEffect(
    () => () => {
      clientRef.current?.dispose();
      clientRef.current = null;
    },
    [],
  );

  /** The editable files as [id, text, path]; ids are positions, valid for the one request they ride on. */
  const entries = useCallback((): [number, string, string][] => {
    const { workspace } = depsRef.current;
    return workspace.state.files.filter((file) => file.editable).map((file, index) => [index, workspace.textOf(file.path), file.path]);
  }, []);

  const run = useCallback(async (): Promise<void> => {
    const current = queryRef.current;
    const mine = (token.current += 1);
    if (current.query === "") {
      setResult(null);
      setStatus("idle");
      setMessage("");
      setNotice("");
      setAnswered((value) => value + 1);
      return;
    }
    setStatus("searching");
    const outcome = await client().search(entries(), current);
    if (outcome === null || mine !== token.current) {
      return;
    }
    if (!outcome.ok) {
      setResult(null);
      setStatus("error");
      setMessage(outcome.message);
      setAnswered((value) => value + 1);
      return;
    }
    const value = outcome.value;
    setResult(value);
    setStatus("done");
    setMessage("");
    setAnswered((count) => count + 1);
  }, [entries]);

  const files = deps.workspace.state.files;
  useEffect(() => {
    if (!deps.visible) {
      return;
    }
    const timer = setTimeout(() => void run(), SEARCH_DEBOUNCE_MS);
    return () => clearTimeout(timer);
  }, [deps.visible, query, files, run]);

  const wrote = (paths: readonly string[]): void => {
    depsRef.current.onWritten(paths);
    setTick((value) => value + 1);
  };

  const replace = useCallback(
    async (only: { path: string; offset: number }[] | null, scope: string): Promise<void> => {
      const { workspace } = depsRef.current;
      const snapshot = entries();
      const idOf = new Map(snapshot.map(([id, , path]) => [path, id]));
      const pathOf = new Map(snapshot.map(([id, , path]) => [id, path]));
      const picks = only === null ? null : only.flatMap((pick) => (idOf.has(pick.path) ? [{ file: idOf.get(pick.path) as number, offset: pick.offset }] : []));
      const text = replacementRef.current;
      const asked = queryRef.current;
      const outcome = await client().replace(snapshot, asked, text, picks);
      if (outcome === null) {
        return;
      }
      if (!outcome.ok) {
        setMessage(outcome.message);
        setNotice(outcome.message);
        return;
      }
      const byPath = new Map<string, TextEdit[]>();
      for (const edit of outcome.value.edits) {
        const path = pathOf.get(edit.file);
        if (path !== undefined) {
          byPath.set(path, [...(byPath.get(path) ?? []), edit]);
        }
      }
      const changes: FileChange[] = [];
      const original = new Map(snapshot.map(([, body, path]) => [path, body]));
      for (const [path, edits] of byPath) {
        const before = workspace.textOf(path);
        if (before !== original.get(path)) {
          const note = "A file changed while replacing, so nothing was replaced. Search again.";
          setMessage(note);
          setNotice(note);
          void run();
          return;
        }
        const after = applyOriginalEdits(before, edits);
        if (after !== before) {
          changes.push({ path, before, after });
        }
      }
      if (changes.length === 0) {
        setNotice("Nothing to replace.");
        void run();
        return;
      }
      const count = outcome.value.edits.length;
      history.push({ label: replaceLabel(asked.query, text, count, changes.length), changes });
      for (const change of changes) {
        workspace.writeBuffer(change.path, change.after);
      }
      setMessage("");
      setNotice(`Replaced ${count} ${count === 1 ? "match" : "matches"} in ${changes.length} ${changes.length === 1 ? "file" : "files"}${scope}. Undo is available.`);
      wrote(changes.map((change) => change.path));
      void run();
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [entries, run, history],
  );

  const stepHistory = (kind: "undo" | "redo"): void => {
    const { workspace } = depsRef.current;
    const done = history[kind](workspace.textOf);
    if (done === null) {
      return;
    }
    for (const write of done.writes) {
      workspace.writeBuffer(write.path, write.text);
    }
    const verb = kind === "undo" ? "Undid" : "Redid";
    const skipped = done.skipped.length === 0 ? "" : ` Left alone because they changed since: ${done.skipped.join(", ")}.`;
    setNotice(`${verb} ${done.label}.${skipped}`);
    wrote(done.writes.map((write) => write.path));
    void run();
  };

  return {
    query,
    replacement,
    showReplace,
    showFilters,
    status,
    result,
    message,
    notice,
    answered,
    collapsed,
    canUndo: history.canUndo(),
    canRedo: history.canRedo(),
    undoLabel: history.undoLabel(),
    redoLabel: history.redoLabel(),
    setQuery: (patch) => {
      setNotice("");
      setQueryState((previous) => ({ ...previous, ...patch }));
    },
    setReplacement,
    setShowReplace,
    setShowFilters,
    toggleFile: (path) =>
      setCollapsed((previous) => {
        const next = new Set(previous);
        if (!next.delete(path)) {
          next.add(path);
        }
        return next;
      }),
    refresh: () => void run(),
    open(path, match) {
      const { workspace, reveal } = depsRef.current;
      // The results are a picture of the project when the search ran. If the text under the match changed
      // since, revealing the stored span would select the wrong characters: search again instead.
      const shown = match.text.slice(match.start, match.end);
      if (!workspace.textOf(path).startsWith(shown, match.offset)) {
        setNotice("The file changed since this result; searching again.");
        void run();
        return;
      }
      reveal({ key: 0, severity: "info", code: "", message: "", file: path, line: match.line, column: match.column, offset: match.offset, length: match.length });
    },
    replaceMatch: (path, match) => void replace([{ path, offset: match.offset }], ""),
    replaceFile(path) {
      const group = resultRef.current?.files.find((candidate) => candidate.path === path);
      if (group !== undefined) {
        void replace(group.matches.map((match) => ({ path, offset: match.offset })), ` (${path})`);
      }
    },
    replaceAll() {
      // A capped list is not "all": the plan would reach matches the person never saw.
      if (resultRef.current === null || resultRef.current.truncated !== null || resultRef.current.total === 0) {
        return;
      }
      void replace(null, "");
    },
    undo: () => stepHistory("undo"),
    redo: () => stepHistory("redo"),
  };
}

/** The one-line summary of a result, shared by the panel and the live region. */
export function summaryOf(result: SearchResult): string {
  const files = result.files.length;
  const base =
    result.total === 0
      ? "No results found."
      : `${result.total}${result.truncated === "results" ? "+" : ""} ${result.total === 1 ? "result" : "results"} in ${files} ${files === 1 ? "file" : "files"}`;
  const notes: string[] = [];
  if (result.truncated === "results") notes.push("showing the first results; narrow the search");
  else if (result.truncated === "time" || result.truncated === "budget") notes.push("the search stopped early; narrow it");
  if (result.skippedBinary > 0) notes.push(`${result.skippedBinary} binary skipped`);
  if (result.skippedLarge > 0) notes.push(`${result.skippedLarge} large skipped`);
  if (result.longLines > 0) notes.push(`${result.longLines} very long ${result.longLines === 1 ? "line" : "lines"} skipped`);
  return notes.length === 0 ? base : `${base} (${notes.join("; ")})`;
}

