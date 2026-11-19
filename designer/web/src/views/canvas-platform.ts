// SPDX-License-Identifier: MIT

// The page-side `platform.event` call (issue #446; `designer-protocol-v1.md` section 5.1).
//
// Like every other page-to-engine call it is one message through `callExport`: the strings are copied into
// the module's heap and freed on the way out, the reply document is released with `tessera_free`, and a
// null return is an error. The engine side is `tessera_canvas_platform_event` (`wasm_canvas_platform.cpp`),
// which runs the same driver as the native scripted run -- there is no preview-only handler to keep in step.
//
// No DOM, no React: this takes the module it is handed, so the Node gate can drive it with the real module.

import { callExport, type WasmModule } from "../lib/wasm-call";
import {
  parseReply,
  unavailableReply,
  type PlatformEventRequest,
  type PlatformReply,
} from "../lib/platform-events";

/** The module export this call needs; a build older than #446 does not have it. */
export const PLATFORM_EXPORT = "_tessera_canvas_platform_event";

/** Whether `module` can deliver platform events. */
export function supportsPlatformEvents(module: WasmModule): boolean {
  return typeof module[PLATFORM_EXPORT] === "function";
}

/**
 * Delivers one event and returns the engine's reply. A call that could not be made is an `ok:false` reply
 * with the `TESSERA_PLATFORM_UNAVAILABLE` code; the engine's own refusals carry its `TESSERA_PLATFORM_*`
 * codes, so the log shows the two apart.
 */
export function sendPlatformEvent(module: WasmModule, request: PlatformEventRequest): PlatformReply {
  if (!supportsPlatformEvents(module)) {
    return unavailableReply(request.type, "this build of the preview engine has no platform events");
  }
  try {
    // A negative timestamp means "now": the engine's clock does not move.
    const text = callExport(module, PLATFORM_EXPORT, [request.type, request.value], [request.timestampNs]);
    return parseReply(request.type, text);
  } catch (error) {
    return unavailableReply(request.type, String(error));
  }
}
