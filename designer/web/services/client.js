// SPDX-License-Identifier: MIT

// The page side of a studio service: versioned requests, latest-wins cancellation, stale-response
// drop, the capability handshake and restart from the next snapshot. UI-free (spec sections 4 to 7).

import { ERROR_CODES, PROTOCOL_VERSION, validateMessage } from './protocol.js';

export class ServiceError extends Error {
  constructor(code, message) {
    super(message);
    this.code = code;
  }
}

/**
 * @typedef {{ post: (m: object, transfer?: ArrayBuffer[]) => void, onMessage: (f: (m: any) => void) => void,
 *   onError: (f: (s: string) => void) => void, terminate: () => void }} WorkerHandle
 */

export class ServiceClient {
  /** @param {{ name: string, createWorker: () => WorkerHandle }} options */
  constructor({ name, createWorker }) {
    this.name = name;
    this.createWorker = createWorker;
    this.version = 0; // the project version: the page owns the text and bumps it on every change
    this.files = new Map(); // file id -> text (UTF-16 string)
    this.paths = new Map(); // file id -> project path, for the files that were given one
    this.capabilities = null;
    this.nextId = 1;
    this.open = new Map(); // id -> { type, version, resolve, reject }
    this.latestByType = new Map(); // type -> id
    this.worker = null;
    this.failure = null;
  }

  /** Starts the worker and runs the capability handshake. */
  async start() {
    this.worker = this.createWorker();
    this.failure = null;
    this.worker.onMessage((m) => this.#receive(m));
    this.worker.onError((text) => {
      this.failure = text;
      this.#rejectAll(new ServiceError(ERROR_CODES.cancelled, `worker failed: ${text}`));
    });
    this.capabilities = await this.#send('capabilities', {});
    return this.capabilities;
  }

  /** True when the service advertised `feature`; a missing feature is not a failure. */
  supports(feature) {
    return this.capabilities?.features.includes(feature) === true;
  }

  /**
   * Replaces every file; the buffers are transferred, the page keeps its own strings. An entry is
   * [id, text] or [id, text, path]: a service that resolves imports (analysis) needs the path.
   */
  async setSnapshot(files) {
    const entries = [...files];
    this.files = new Map(entries.map(([id, text]) => [id, text]));
    this.paths = new Map(entries.filter((e) => typeof e[2] === 'string').map(([id, , path]) => [id, path]));
    this.version += 1;
    return this.#sendSnapshot();
  }

  /** Applies edits (UTF-16 offsets) to the page's text, then sends the delta against the old version. */
  async applyEdits(edits) {
    const baseVersion = this.version;
    for (const e of edits) {
      const text = this.files.get(e.file) ?? '';
      this.files.set(e.file, text.slice(0, e.offset) + e.newText + text.slice(e.offset + e.length));
    }
    this.version += 1;
    return this.#send('delta', { baseVersion, edits });
  }

  /**
   * A service request against the current project version. A newer request of the same type
   * supersedes the older one (it rejects with TESSERA_STUDIO_CANCELLED and the worker is told).
   */
  request(type, payload = {}) {
    const previous = this.latestByType.get(type);
    if (previous !== undefined && this.open.has(previous)) this.#cancel(previous);
    const promise = this.#send(type, payload);
    this.latestByType.set(type, this.nextId - 1);
    return promise;
  }

  /** Terminates the worker and starts a new one; it rebuilds from the page's current text. */
  async restart() {
    this.worker?.terminate();
    this.#rejectAll(new ServiceError(ERROR_CODES.cancelled, 'worker restarted'));
    const caps = await this.start();
    await this.#sendSnapshot();
    return caps;
  }

  dispose() {
    this.worker?.terminate();
    this.#rejectAll(new ServiceError(ERROR_CODES.cancelled, 'client disposed'));
  }

  #sendSnapshot() {
    const encoder = new TextEncoder();
    const files = [...this.files].map(([file, text]) => {
      const entry = { file, bytes: encoder.encode(text).buffer };
      if (this.paths.has(file)) entry.path = this.paths.get(file);
      return entry;
    });
    return this.#send('snapshot', { files }, files.map((f) => f.bytes));
  }

  #send(type, payload, transfer = []) {
    const id = this.nextId++;
    const message = { v: PROTOCOL_VERSION, kind: 'request', service: this.name, type, id, version: this.version, payload };
    const problems = validateMessage(message);
    if (problems.length > 0) return Promise.reject(new ServiceError(ERROR_CODES.malformed, problems.join('; ')));
    return new Promise((resolve, reject) => {
      this.open.set(id, { type, version: this.version, resolve, reject });
      this.worker.post(message, transfer);
    });
  }

  #cancel(id) {
    const entry = this.open.get(id);
    this.open.delete(id);
    entry.reject(new ServiceError(ERROR_CODES.cancelled, `${entry.type} superseded`));
    const cancelId = this.nextId++;
    this.worker.post({ v: PROTOCOL_VERSION, kind: 'request', service: this.name, type: 'cancel', id: cancelId, version: this.version, payload: { target: id } });
  }

  #receive(m) {
    if (validateMessage(m).length > 0 || m.kind !== 'response') return;
    const entry = this.open.get(m.id);
    if (!entry) return; // already cancelled or superseded: the answer is dropped
    this.open.delete(m.id);
    if (m.payload.error) return entry.reject(new ServiceError(m.payload.error.code, m.payload.error.message));
    // Responses older than the text the page shows are dropped, not delivered late. The snapshot,
    // delta and handshake requests answer for the version they set, so they are exempt.
    if (m.version < this.version && !['snapshot', 'delta', 'capabilities'].includes(entry.type)) {
      return entry.reject(new ServiceError(ERROR_CODES.stale, `${entry.type} answered version ${m.version}, page is at ${this.version}`));
    }
    entry.resolve(m.payload.result);
  }

  #rejectAll(error) {
    const open = [...this.open.values()];
    this.open.clear();
    for (const e of open) e.reject(error);
  }
}
