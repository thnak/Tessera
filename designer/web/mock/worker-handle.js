// SPDX-License-Identifier: MIT

// One mock worker, behind the smallest interface the host needs.
//
// `createWorker(source)` is injected so the host logic (`host.js`) is the same code in the page (a blob
// `Worker`, `browserWorker` below) and in the Node gate (a `worker_threads` worker). A handle is a
// one-shot object: `post` a message, listen with `onMessage`/`onError`, `terminate` it. The host never
// reuses a terminated handle; a restart builds a new one from the same source.

/**
 * @typedef {{
 *   post: (message: object) => void,
 *   onMessage: (listener: (message: any) => void) => void,
 *   onError: (listener: (message: string) => void) => void,
 *   terminate: () => void,
 * }} MockWorkerHandle
 */

/**
 * The browser implementation: a dedicated worker from a blob URL.
 *
 * The blob is classic (not `type: 'module'`) because the prelude and the IIFE-compiled mock are one
 * classic script. The document's CSP must allow it (`worker-src 'self' blob:`, spec section 9); a page
 * without that allowance fails here with a worker error, which the host reports instead of running the
 * mock anywhere else.
 *
 * @param {string} source
 * @returns {MockWorkerHandle}
 */
export function browserWorker(source) {
  const url = URL.createObjectURL(new Blob([source], { type: 'text/javascript' }));
  const worker = new Worker(url);
  return {
    post: (message) => worker.postMessage(message),
    onMessage: (listener) => worker.addEventListener('message', (event) => listener(event.data)),
    onError: (listener) =>
      worker.addEventListener('error', (event) => listener(event.message || 'worker error')),
    terminate: () => {
      worker.terminate();
      URL.revokeObjectURL(url);
    },
  };
}
