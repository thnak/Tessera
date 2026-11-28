// SPDX-License-Identifier: MIT

// A studio service worker as the protocol client's handle (docs/specs/studio-services-v1.md section 9): the
// service source runs in a blob Worker, so no file and no cross-origin script is needed. Shared by the
// format, analysis and compile clients.

export interface BrowserWorkerHandle {
  post(message: object, transfer?: ArrayBuffer[]): void;
  onMessage(listener: (message: unknown) => void): void;
  onError(listener: (message: string) => void): void;
  terminate(): void;
}

export function browserWorker(source: string): BrowserWorkerHandle {
  const url = URL.createObjectURL(new Blob([source], { type: "text/javascript" }));
  const worker = new Worker(url);
  return {
    post: (message, transfer = []) => worker.postMessage(message, transfer),
    onMessage: (listener) => worker.addEventListener("message", (event) => listener(event.data)),
    onError: (listener) => worker.addEventListener("error", (event) => listener(event.message || "worker error")),
    terminate: () => {
      worker.terminate();
      URL.revokeObjectURL(url);
    },
  };
}
