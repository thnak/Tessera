// SPDX-License-Identifier: MIT

// The page's side of the `search` service (#580, ADR-060): the project goes to a worker as a snapshot and a
// search or a replace plan comes back as plain data with UTF-16 spans. The worker starts on the first use;
// a newer request of a type supersedes the older one (the answer is `null`, never a stale list), and a worker
// that died is restarted from the next snapshot. Pattern problems come back as a sentence, not a throw.

import { ServiceClient } from "../../services/client.js";
import { searchServiceSource } from "../../services/search-service.js";
import { browserWorker, type BrowserWorkerHandle } from "./service-worker";

/** What the person typed: the query and its options. */
export interface SearchQuery {
  query: string;
  caseSensitive: boolean;
  wholeWord: boolean;
  regex: boolean;
  /** Comma-separated globs; empty means every file. */
  include: string;
  exclude: string;
}

export interface SearchMatch {
  offset: number;
  length: number;
  line: number;
  column: number;
  /** The line, trimmed and windowed around the match, and the match's span inside it. */
  text: string;
  start: number;
  end: number;
}

export interface SearchFileResult {
  file: number;
  path: string;
  matches: SearchMatch[];
}

export interface SearchResult {
  files: SearchFileResult[];
  total: number;
  /** Why the walk stopped early; `null` when it covered every file. */
  truncated: null | "results" | "time" | "budget" | "cancelled";
  searched: number;
  skippedBinary: number;
  skippedLarge: number;
  longLines: number;
}

export interface ReplaceEdit {
  file: number;
  offset: number;
  length: number;
  newText: string;
}

export interface ReplacePlan {
  edits: ReplaceEdit[];
  files: number;
  truncated: null | string;
}

/** An answer, or the sentence that says why there is none (a bad pattern, an unavailable worker). */
export type Outcome<T> = { ok: true; value: T } | { ok: false; message: string };

export interface SearchClient {
  /** `null` when a newer search superseded this one. `entries` are [id, text, path]. */
  search(entries: readonly [number, string, string][], query: SearchQuery, maxResults?: number): Promise<Outcome<SearchResult> | null>;
  /** The replace plan for `only` (null: every match). */
  replace(
    entries: readonly [number, string, string][],
    query: SearchQuery,
    replacement: string,
    only: readonly { file: number; offset: number }[] | null,
  ): Promise<Outcome<ReplacePlan> | null>;
  dispose(): void;
}

const SUPERSEDED = new Set(["TESSERA_STUDIO_CANCELLED", "TESSERA_STUDIO_STALE"]);

export function createSearchClient(
  createWorker: () => BrowserWorkerHandle = () => browserWorker(searchServiceSource()),
): SearchClient {
  const service = new ServiceClient({ name: "search", createWorker });
  let starting: Promise<unknown> | null = null;

  const ask = async <T>(
    entries: readonly [number, string, string][],
    type: "search" | "replace",
    payload: Record<string, unknown>,
  ): Promise<Outcome<T> | null> => {
    try {
      starting ??= service.start().catch((error) => {
        starting = null;
        throw error;
      });
      await starting;
      const value = (await service.withRecovery(async () => {
        await service.setSnapshot(entries);
        return service.request(type, payload);
      })) as T;
      return { ok: true, value };
    } catch (error) {
      const code = (error as { code?: string }).code ?? "";
      if (SUPERSEDED.has(code)) {
        return null;
      }
      return { ok: false, message: String((error as Error).message ?? error) };
    }
  };

  const base = (query: SearchQuery): Record<string, unknown> => ({
    query: query.query,
    caseSensitive: query.caseSensitive,
    wholeWord: query.wholeWord,
    regex: query.regex,
    include: query.include,
    exclude: query.exclude,
  });

  return {
    search: (entries, query, maxResults) =>
      ask<SearchResult>(entries, "search", maxResults === undefined ? base(query) : { ...base(query), maxResults }),
    replace: (entries, query, replacement, only) => ask<ReplacePlan>(entries, "replace", { ...base(query), replacement, only }),
    dispose: () => service.dispose(),
  };
}
