// SPDX-License-Identifier: MIT

// The page's side of the `compile` service (#569, ADR-060): the Diagnostics view asks a worker for the
// compiler's diagnostics document instead of calling Wasm exports from the page. The worker is started on
// the first request (the module loads there, not at startup); a worker that died is restarted from the
// next snapshot, so a kill costs one cold start and shows no page error.

import { ServiceClient } from "../../services/client.js";
import { compileServiceSource, wasmEngineSource } from "../../services/compile-service.js";
import type { DiagnosticsDocument } from "../views/diagnostics-render";
import { wasmModuleUrl } from "./module-url";
import { browserWorker, type BrowserWorkerHandle } from "./service-worker";
import type { StagedProject } from "./wasm-call";

export interface CompileClient {
  /** Starts the worker and its handshake; resolves when the service answered (the module loads on the first compile). */
  start(): Promise<void>;
  /** Compiles `project` in the worker and returns the diagnostics document `tessera_diagnostics_json` wrote. */
  diagnostics(project: StagedProject): Promise<DiagnosticsDocument>;
  dispose(): void;
}

/** `createWorker` is injectable for tests; the page passes nothing and gets the blob worker around the Wasm engine. */
export function createCompileClient(
  createWorker: () => BrowserWorkerHandle = () =>
    browserWorker(compileServiceSource(wasmEngineSource(wasmModuleUrl("tessera_diagnostics.js")))),
): CompileClient {
  const service = new ServiceClient({ name: "compile", createWorker });
  let starting: Promise<void> | null = null;

  const start = (): Promise<void> => {
    starting ??= service.start().then(
      () => undefined,
      (error) => {
        starting = null;
        throw error;
      },
    );
    return starting;
  };

  return {
    start,
    async diagnostics(project) {
      await start();
      const entries: [number, string, string][] = [...project.sources, ...project.stylesheets].map((file, index) => [
        index,
        file.text,
        file.path,
      ]);
      const answer = (await service.withRecovery(async () => {
        await service.setSnapshot(entries);
        return service.request("diagnostics", { entry: project.entry, servicesManifest: project.servicesManifest ?? null });
      })) as { document: DiagnosticsDocument };
      return answer.document;
    },
    dispose: () => service.dispose(),
  };
}
