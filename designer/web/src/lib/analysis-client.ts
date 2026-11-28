// SPDX-License-Identifier: MIT

// The page's side of the `analysis` service for the Problems panel (#575, ADR-060): the whole project goes to
// the worker as a snapshot, and its `diagnostics` answer for every file becomes Problems rows. The worker
// loads `tessera_analysis.wasm` lazily on the first snapshot; a site without the module answers an engine
// error, which marks the client unavailable (no retry per keystroke) and leaves the panel on the compile rows.

import { ServiceClient } from "../../services/client.js";
import { analysisServiceSource, wasmEngineSource } from "../../services/analysis-service.js";
import type { Problem } from "./problems";
import { analysisEntries, problemsFromAnalysis, type AnalysisDiagnostics, type AnalysisEntry } from "./project-problems";
import { wasmModuleUrl } from "./module-url";
import { browserWorker, type BrowserWorkerHandle } from "./service-worker";

export interface AnalysisClient {
  /** The project-wide rows, or `null` when the analysis is unavailable or the answer was superseded by a newer one. */
  problems(
    files: readonly { path: string; kind: string }[],
    textOf: (path: string) => string,
  ): Promise<Problem[] | null>;
  /**
   * One position request (`definition`, `references`, `hover`, `completion`, `prepareRename`, `rename`) against `entries`.
   * `null` when a newer request of the type superseded it; the sentence in `message` when the worker is unavailable.
   * An unchanged project is not sent again, so a hover or a completion costs one request, not a snapshot.
   */
  query(entries: readonly AnalysisEntry[], type: string, payload: Record<string, unknown>): Promise<Outcome<unknown> | null>;
  /** False once the worker proved unavailable (no module on this site): the editor features stay quiet. */
  available(): boolean;
  dispose(): void;
}

/** An answer, or the sentence that says why there is none. */
export type Outcome<T> = { ok: true; value: T } | { ok: false; message: string };

const sameEntries = (a: readonly AnalysisEntry[] | null, b: readonly AnalysisEntry[]): boolean =>
  a !== null && a.length === b.length && a.every((entry, index) => entry[0] === b[index][0] && entry[2] === b[index][2] && entry[1] === b[index][1]);

export function createAnalysisClient(
  createWorker: () => BrowserWorkerHandle = () =>
    browserWorker(analysisServiceSource(wasmEngineSource(wasmModuleUrl("tessera_analysis.js")))),
): AnalysisClient {
  const service = new ServiceClient({ name: "analysis", createWorker });
  let starting: Promise<unknown> | null = null;
  let unavailable = false;
  let sent: readonly AnalysisEntry[] | null = null;

  return {
    async problems(files, textOf) {
      if (unavailable) {
        return null;
      }
      const entries = analysisEntries(files, textOf);
      try {
        starting ??= service.start();
        await starting;
        const answer = await service.withRecovery(async () => {
          await service.setSnapshot(entries);
          sent = entries;
          return service.request("diagnostics");
        });
        return problemsFromAnalysis(answer as AnalysisDiagnostics, entries);
      } catch (error) {
        const code = (error as { code?: string }).code;
        if (code === "TESSERA_STUDIO_CANCELLED" || code === "TESSERA_STUDIO_STALE") {
          return null;
        }
        unavailable = true;
        return null;
      }
    },
    async query(entries, type, payload) {
      if (unavailable) {
        return { ok: false, message: "Code intelligence is not available on this site." };
      }
      try {
        starting ??= service.start();
        await starting;
        const value = await service.withRecovery(async () => {
          if (!sameEntries(sent, entries)) {
            await service.setSnapshot(entries);
            sent = entries;
          }
          return service.request(type, payload);
        });
        return { ok: true, value };
      } catch (error) {
        const code = (error as { code?: string }).code ?? "";
        if (code === "TESSERA_STUDIO_CANCELLED" || code === "TESSERA_STUDIO_STALE") {
          sent = null;
          return null;
        }
        if (code !== "" && code !== "TESSERA_ANALYSIS_ENGINE") {
          return { ok: false, message: String((error as Error).message ?? error) };
        }
        unavailable = true;
        return { ok: false, message: "Code intelligence is not available on this site." };
      }
    },
    available: () => !unavailable,
    dispose: () => service.dispose(),
  };
}
