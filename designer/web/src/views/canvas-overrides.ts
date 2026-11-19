// SPDX-License-Identifier: MIT

// The canvas session's wiring of the override layer (issue #585): the client bound to the module, the
// `CommitHost` a commit needs, and the registration of the override stack with the page's Undo/Redo routing.
//
// **What "staged" means here.** The module computes a patch's offsets in the text it has staged, and the page
// restages the whole project on every frame. This wiring remembers the text of each file as of the last
// evaluation that SUCCEEDED, so a patch is only applied while the buffer still equals it: a keystroke inside
// the live-edit debounce, or a source that does not compile, makes `isCurrent` false and the commit is
// rejected with `version_mismatch` instead of landing at stale offsets.
//
// **Evaluation waiters.** The round trip's last step (settle) is only meaningful after the new source was
// evaluated, and evaluation happens inside the frame callback, not inside `applyNow()`'s promise. So a commit
// registers a waiter that the frame resolves once it has applied the source (or failed to).

import {
  createOverrideClient,
  type CommitBridge,
  type CommitHost,
  type CommitOutcome,
  type OverrideClient,
} from "../lib/override-client";
import type { OverrideDomainHost } from "../lib/edit-routing";
import type { ProjectReader } from "../lib/views";
import { callExport, type StagedProject, type WasmModule } from "../lib/wasm-call";

/** How long a commit waits for the frame that evaluates its new source before it reports `unsettled`. */
const EVALUATION_TIMEOUT_MS = 4000;

export interface CanvasOverrides {
  client: OverrideClient;
  /** The commit host, or `null` when the page supplied no bridge. */
  host: () => CommitHost | null;
  /** The text the module evaluated successfully, recorded after each good apply. */
  staged: (project: StagedProject) => void;
  /** A frame finished applying the source: resolves commits waiting for it, and settles a late one. */
  evaluated: (ok: boolean) => CommitOutcome | null;
  /** The module's session ended (restart, relaunch): overrides and history are gone. */
  ended: () => void;
  dispose: () => void;
}

export function createCanvasOverrides(options: {
  module: () => WasmModule | null;
  commit?: CommitBridge;
  editing?: OverrideDomainHost;
  source: ProjectReader;
  requestFrame: () => void;
}): CanvasOverrides {
  const client = createOverrideClient({
    invoke: (name, strings) => {
      const module = options.module();
      if (module === null) {
        throw new Error("the canvas module is not loaded");
      }
      return callExport(module, name, strings);
    },
  });
  const staged = new Map<string, string>();
  let waiters: Array<(ok: boolean) => void> = [];

  options.editing?.setOverrideDomain({
    state: () => {
      const state = client.state();
      return {
        canUndo: state.canUndo,
        canRedo: state.canRedo,
        undoLabel: state.undoLabel,
        redoLabel: state.redoLabel,
      };
    },
    undo: () => {
      const result = client.undo();
      if (result.changed) {
        options.requestFrame();
      }
      return result.changed;
    },
    redo: () => {
      const result = client.redo();
      if (result.changed) {
        options.requestFrame();
      }
      return result.changed;
    },
    subscribe: client.subscribe,
  });

  const bridge = options.commit;
  const host: CommitHost | null =
    bridge === undefined
      ? null
      : {
          text: (path) => bridge.text(path),
          isCurrent: (path) => {
            // The theme asset is handed over at commit time with the buffer's own text, so it is current by
            // construction; every other file must still equal what the last good evaluation staged.
            const theme = options.source().theme;
            return (theme !== undefined && theme.path === path) || staged.get(path) === bridge.text(path);
          },
          applyEdits: (path, changes, label) => bridge.applyEdits(path, changes, label),
          theme: () => options.source().theme ?? null,
          evaluate: () =>
            new Promise<boolean>((resolve) => {
              const timer = setTimeout(() => finish(false), EVALUATION_TIMEOUT_MS);
              const finish = (ok: boolean): void => {
                clearTimeout(timer);
                waiters = waiters.filter((waiter) => waiter !== finish);
                resolve(ok);
              };
              waiters.push(finish);
              void bridge.applyNow();
              options.requestFrame();
            }),
        };

  return {
    client,
    host: () => host,
    staged: (project) => {
      staged.clear();
      for (const file of [...project.sources, ...project.stylesheets]) {
        staged.set(file.path, file.text);
      }
    },
    evaluated: (ok) => {
      const pending = waiters;
      waiters = [];
      for (const waiter of pending) {
        waiter(ok);
      }
      // A commit that applied while the source did not compile waits for the next good evaluation.
      return ok && client.pendingSettle() !== null ? client.evaluated() : null;
    },
    ended: () => client.reset(),
    dispose: () => {
      options.editing?.setOverrideDomain(null);
      client.reset();
    },
  };
}
