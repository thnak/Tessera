// SPDX-License-Identifier: MIT

// The page's side of the `analysis` service for the Problems panel (#575, ADR-060): the whole project goes to
// the worker as a snapshot, and its `diagnostics` answer for every file becomes Problems rows. The worker
// loads `tessera_analysis.wasm` lazily on the first snapshot; a site without the module answers an engine
// error, which marks the client unavailable (no retry per keystroke) and leaves the panel on the compile rows.

import { ServiceClient } from "../../services/client.js";
import { analysisServiceSource, wasmEngineSource } from "../../services/analysis-service.js";
import type { Problem } from "./problems";
import { analysisEntries, problemsFromAnalysis, type AnalysisDiagnostics } from "./project-problems";
import { wasmModuleUrl } from "./module-url";
import { browserWorker, type BrowserWorkerHandle } from "./service-worker";

export interface AnalysisClient {
  /** The project-wide rows, or `null` when the analysis is unavailable or the answer was superseded by a newer one. */
  problems(
    files: readonly { path: string; kind: string }[],
    textOf: (path: string) => string,
  ): Promise<Problem[] | null>;
  dispose(): void;
}

export function createAnalysisClient(
  createWorker: () => BrowserWorkerHandle = () =>
    browserWorker(analysisServiceSource(wasmEngineSource(wasmModuleUrl("tessera_analysis.js")))),
): AnalysisClient {
  const service = new ServiceClient({ name: "analysis", createWorker });
  let starting: Promise<unknown> | null = null;
  let unavailable = false;

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
    dispose: () => service.dispose(),
  };
}
