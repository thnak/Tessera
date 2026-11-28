// SPDX-License-Identifier: MIT

// The page's feed for the project-wide Problems rows (#575, ADR-060): after the files change it sends the
// project to the analysis worker (debounced, off the page thread) and publishes the rows it answers for every
// file, open or not. Without the module the store keeps the compile rows alone.
//
// Split out of `App.tsx` (the 500-line limit). Typing never waits on it: the worker parses, the page only
// receives the finished rows, and a newer snapshot cancels the one in flight.

import { useCallback, useEffect, useRef } from "react";
import { createAnalysisClient, type AnalysisClient } from "./lib/analysis-client";
import type { ProblemsStore } from "./lib/problems";
import type { Workspace } from "./page-workspace";

/** Longer than the live edit's 150 ms: the panel is a summary, not the thing the person is looking at. */
export const PROJECT_PROBLEMS_DEBOUNCE_MS = 300;

/** Returns `schedule()`, to call on every edit; a change of the file set (open, create, delete) schedules itself. */
export function useProjectProblems(store: ProblemsStore, workspace: Workspace): () => void {
  const latest = useRef(workspace);
  latest.current = workspace;
  const client = useRef<AnalysisClient | null>(null);
  const timer = useRef<number | null>(null);
  const generation = useRef(0);

  const run = useCallback(async () => {
    const { files } = latest.current.state;
    if (files.length === 0) {
      return;
    }
    client.current ??= createAnalysisClient();
    generation.current += 1;
    const mine = generation.current;
    const rows = await client.current.problems(files, latest.current.textOf);
    // A newer run started while this one waited; its rows are the ones to show.
    if (mine === generation.current && rows !== null) {
      store.publishProject(rows);
    }
  }, [store]);

  const schedule = useCallback(() => {
    if (timer.current !== null) {
      clearTimeout(timer.current);
    }
    timer.current = window.setTimeout(() => {
      timer.current = null;
      void run();
    }, PROJECT_PROBLEMS_DEBOUNCE_MS);
  }, [run]);

  const signature = workspace.state.files.map((file) => file.path).join("\n");
  useEffect(() => {
    schedule();
  }, [signature, schedule]);

  useEffect(
    () => () => {
      if (timer.current !== null) {
        clearTimeout(timer.current);
      }
      client.current?.dispose();
      client.current = null;
    },
    [],
  );

  return schedule;
}
