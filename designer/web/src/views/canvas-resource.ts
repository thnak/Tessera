// SPDX-License-Identifier: MIT

// The page-side resource control (issue #657; `designer-protocol-v1.md` section 5.2).
//
// Like every other page-to-engine call it is one message through `callExport`: the strings are copied into
// the module's heap and freed on the way out, the reply is released with `tessera_free`, and a null return
// is an error. The engine side is `tessera_canvas_resource` (`wasm_canvas_entry.cpp`), which drives the
// preview session's fake resource of that name -- there is no preview-only model here.
//
// No DOM, no React: this takes the module it is handed, so the Node gate can drive it with the real module.

import { callExport, type WasmModule } from "../lib/wasm-call";
import { parseReport, unavailableReport, type ResourceReport, type ResourceRequest } from "../lib/resource-controls";

/** The module export this call needs; a build older than #657 does not have it. */
export const RESOURCE_EXPORT = "_tessera_canvas_resource";

/** Whether `module` can drive fake resources. */
export function supportsResourceControl(module: WasmModule): boolean {
  return typeof module[RESOURCE_EXPORT] === "function";
}

/**
 * Sends one control and returns the engine's report. A call that could not be made is an `ok:false` report
 * whose `error` says why, so the panel shows one sentence instead of an exception.
 */
export function sendResourceControl(module: WasmModule, request: ResourceRequest): ResourceReport {
  if (!supportsResourceControl(module)) {
    return unavailableReport("this build of the preview engine has no resource controls");
  }
  try {
    const text = callExport(
      module,
      RESOURCE_EXPORT,
      [request.action, request.resource, request.text],
      [request.number, request.epoch],
    );
    return parseReport(text);
  } catch (error) {
    return unavailableReport(String(error));
  }
}
