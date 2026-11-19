// SPDX-License-Identifier: MIT

// The studio-side mock host (docs/specs/studio-mock-services-v1.md sections 3 to 7): it loads
// `*.mock.ts` files, compiles and starts one sandbox worker per mock, binds each to the service
// manifest, serves recorded calls with a response, and keeps the trace and the response table.
//
// **No DOM, no React, no timers of its own.** The worker factory, the transpiler and the timers are
// injected, so this is the same code under the page (blob worker, esbuild-wasm, `setTimeout`) and under
// the Node gate (`worker_threads`, a stub compile, real timers). Nothing here claims a mock changes a
// previewed frame: there is no engine service seam yet (ADR-055 decision 9), so the host's output is the
// console's data and a `mock.responses` payload nothing consumes.
//
// **Failure isolation.** Every failure is a `MockDiagnostic` on one mock's status (a stable code, a file
// and a position where there is one). A bad, slow or hostile mock disables itself and nothing else: the
// other mocks, the console and the preview keep running (spec section 3.6).

import { MOCK_LIMITS, ResponseTable, argsKeyOf, canonicalJson, validateResponse } from './response.js';
import { checkSubset } from './subset.js';
import { buildWorkerSource } from './sandbox-source.js';
import { emptyTrace, traceBytes } from './trace.js';

const INIT_TIMEOUT_MS = 1000;

/** @typedef {{ code: string, message: string, line?: number, column?: number, severity: 'Error' | 'Warn' }} Diagnostic */
/** @typedef {{ interfaces: Record<string, { methods: string[] }> }} ServiceManifest */

/** SHA-256 of `text` as lowercase hex (Web Crypto, present in the page and in Node). */
export async function sha256Hex(text) {
  const digest = await globalThis.crypto.subtle.digest('SHA-256', new TextEncoder().encode(text));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, '0')).join('');
}

/**
 * The host for one project's mocks.
 *
 * @param {{
 *   createWorker: (source: string) => import('./worker-handle.js').MockWorkerHandle,
 *   compile: (text: string, path: string) => Promise<string>,
 *   seed?: number,
 *   timers?: { setTimeout: Function, clearTimeout: Function, now: () => number },
 * }} deps
 *   `compile` returns the classic-script IIFE the prelude expects, or throws an object
 *   `{ message, line?, column? }`.
 */
export function createMockHost(deps) {
  const timers = deps.timers ?? {
    setTimeout: (fn, ms) => globalThis.setTimeout(fn, ms),
    clearTimeout: (id) => globalThis.clearTimeout(id),
    now: () => globalThis.performance.now(),
  };
  const seed = deps.seed ?? 1;
  const listeners = new Set();
  const table = new ResponseTable();

  /** @type {Map<string, any>} path -> mock record */
  let mocks = new Map();
  let manifest = null;
  let enabled = true;
  let trace = emptyTrace();
  let traceStopped = false;
  let nextSeq = 0;
  let nextCallId = 1;

  const emit = () => {
    for (const listener of listeners) {
      listener();
    }
  };
  const diag = (code, message, extra = {}) => ({ code, message, severity: 'Error', ...extra });

  // --- one worker ---------------------------------------------------------------------------------

  const stop = (mock) => {
    if (mock.worker !== null) {
      mock.worker.terminate();
      mock.worker = null;
    }
    for (const pending of mock.pending.values()) {
      timers.clearTimeout(pending.timer);
      pending.resolve({ type: 'error', code: 'TESSERA_MOCK_TIMEOUT', message: 'The mock was stopped.' });
    }
    mock.pending.clear();
  };

  // Disables a mock with a named reason and stops its worker; the preview and other mocks go on.
  const disable = (mock, code, message) => {
    mock.diagnostics.push(diag(code, message, { file: mock.path }));
    mock.status = 'error';
    mock.disabled = true;
    stop(mock);
  };

  const startWorker = (mock) =>
    new Promise((resolve) => {
      let settled = false;
      const done = (outcome) => {
        if (!settled) {
          settled = true;
          timers.clearTimeout(initTimer);
          resolve(outcome);
        }
      };
      const worker = deps.createWorker(mock.source);
      mock.worker = worker;
      const initTimer = timers.setTimeout(() => {
        done({ type: 'init-error', code: 'TESSERA_MOCK_TIMEOUT', message: 'The mock did not start in time.' });
      }, INIT_TIMEOUT_MS);
      worker.onError((message) =>
        done({ type: 'init-error', code: 'TESSERA_MOCK_TRANSPILE', message: String(message) }),
      );
      worker.onMessage((message) => {
        if (message.type === 'ready' || message.type === 'init-error') {
          done(message);
        } else if (message.type === 'result' || message.type === 'error') {
          const pending = mock.pending.get(message.id);
          if (pending !== undefined) {
            mock.pending.delete(message.id);
            timers.clearTimeout(pending.timer);
            pending.resolve(message);
          }
        }
      });
    });

  // --- load and bind --------------------------------------------------------------------------------

  const bind = (mock) => {
    if (mock.status === 'error') {
      return;
    }
    if (manifest === null) {
      mock.status = 'unbound';
      mock.reason = 'The project has no service manifest (ui/services.json), so no interface can bind.';
      return;
    }
    const known = manifest.interfaces[mock.interface];
    if (known === undefined) {
      mock.status = 'unbound';
      mock.reason = `The service manifest has no interface ${mock.interface}.`;
      mock.diagnostics.push({
        code: 'TESSERA_MOCK_INTERFACE_UNKNOWN',
        message: `The interface ${mock.interface} is not in the service manifest.`,
        severity: 'Warn',
        file: mock.path,
      });
      return;
    }
    const unknown = mock.methods.filter((name) => !known.methods.includes(name));
    if (unknown.length > 0) {
      mock.status = 'error';
      mock.reason = `Unknown method${unknown.length > 1 ? 's' : ''}: ${unknown.join(', ')}.`;
      for (const name of unknown) {
        mock.diagnostics.push(
          diag('TESSERA_MOCK_METHOD_UNKNOWN', `The method ${name} is not in the manifest for ${mock.interface}.`, {
            file: mock.path,
          }),
        );
      }
      return;
    }
    mock.status = 'bound';
    mock.reason = '';
  };

  // Two mocks for one interface: both are unbound until one is removed (spec section 3.5).
  const rejectDuplicates = () => {
    const byInterface = new Map();
    for (const mock of mocks.values()) {
      if (mock.interface !== '' && mock.status !== 'error') {
        byInterface.set(mock.interface, [...(byInterface.get(mock.interface) ?? []), mock]);
      }
    }
    for (const [iface, group] of byInterface) {
      if (group.length > 1) {
        for (const mock of group) {
          disable(mock, 'TESSERA_MOCK_DUPLICATE_INTERFACE', `Another mock also implements ${iface}.`);
          mock.reason = `Duplicate implementation of ${iface}.`;
        }
      }
    }
  };

  const loadOne = async (file) => {
    const mock = {
      path: file.path,
      version: file.version ?? 0,
      sha256: await sha256Hex(file.text),
      status: 'error',
      reason: '',
      interface: '',
      methods: [],
      diagnostics: [],
      source: '',
      worker: null,
      pending: new Map(),
      calls: 0,
      cpuMs: 0,
      disabled: false,
    };
    const findings = (deps.checkSubset ?? checkSubset)(file.text);
    if (findings.length > 0) {
      mock.diagnostics = findings.map((f) => ({ ...f, severity: 'Error', file: file.path }));
      mock.reason = `${findings[0].code} at ${findings[0].line}:${findings[0].column}`;
      return mock;
    }
    let compiled;
    try {
      compiled = await deps.compile(file.text, file.path);
    } catch (cause) {
      mock.diagnostics = [
        diag('TESSERA_MOCK_TRANSPILE', String(cause.message ?? cause), {
          file: file.path,
          line: cause.line,
          column: cause.column,
        }),
      ];
      mock.reason = 'TESSERA_MOCK_TRANSPILE';
      return mock;
    }
    mock.source = buildWorkerSource(compiled, { seed });
    const started = await startWorker(mock);
    if (started.type === 'init-error') {
      mock.diagnostics = [diag(started.code, started.message, { file: file.path })];
      mock.reason = started.code;
      stop(mock);
      return mock;
    }
    mock.interface = started.interface;
    mock.methods = started.methods;
    mock.status = 'unbound';
    return mock;
  };

  /**
   * Replaces the project's mocks and binds them. `files` are `{ path, text, version }` of the project's
   * `mock` kind files; `serviceManifest` is `null` until the manifest spec exists.
   *
   * @param {{ path: string, text: string, version?: number }[]} files
   * @param {ServiceManifest | null} serviceManifest
   */
  async function load(files, serviceManifest) {
    for (const mock of mocks.values()) {
      stop(mock);
    }
    manifest = serviceManifest;
    const next = new Map();
    if (files.length > 64) {
      files = files.slice(0, 64);
    }
    for (const file of files) {
      next.set(file.path, await loadOne(file));
    }
    mocks = next;
    rejectDuplicates();
    for (const mock of mocks.values()) {
      bind(mock);
      if (mock.status !== 'bound') {
        stop(mock);
      }
    }
    emit();
  }

  // --- calls ----------------------------------------------------------------------------------------

  const record = (entry) => {
    if (traceStopped) {
      return;
    }
    trace.entries.push(entry);
    if (
      trace.entries.length > MOCK_LIMITS.maxTraceEntries ||
      traceBytes(trace) > MOCK_LIMITS.maxTraceBytes
    ) {
      trace.entries.pop();
      traceStopped = true;
      trace.stopped = 'TESSERA_MOCK_TRACE_LIMIT';
    }
  };

  const mockFor = (iface) => {
    for (const mock of mocks.values()) {
      if (mock.interface === iface && mock.status === 'bound' && !mock.disabled) {
        return mock;
      }
    }
    return null;
  };

  const callWorker = (mock, method, args, simTimeMs, callIndex) =>
    new Promise((resolve) => {
      const id = nextCallId++;
      const timer = timers.setTimeout(() => {
        mock.pending.delete(id);
        resolve({ type: 'error', code: 'TESSERA_MOCK_TIMEOUT', message: 'The handler ran over 50 ms.' });
      }, MOCK_LIMITS.maxCpuMsPerCall);
      mock.pending.set(id, { resolve, timer });
      mock.worker.post({ type: 'call', id, method, args, nowMs: simTimeMs, callIndex });
    });

  /**
   * Serves one call the engine-side fake recorded (or the console issued) and records it in the trace.
   *
   * @returns {Promise<object>} the trace entry: `{ seq, interface, method, args, response, state, simTimeMs, delayMs }`
   */
  async function call(iface, method, args, simTimeMs) {
    const seq = nextSeq++;
    const entry = { seq, interface: iface, method, args, response: null, state: 'unmocked', simTimeMs, delayMs: null };
    const finish = (extra) => {
      Object.assign(entry, extra);
      record(entry);
      emit();
      return entry;
    };
    const argsKey = argsKeyOf(args);
    if (argsKey === null) {
      return finish({
        state: 'error',
        response: { error: { code: 'TESSERA_MOCK_LIMIT', message: 'The call arguments are not JSON or exceed 8 KiB.' } },
      });
    }
    const mock = enabled ? mockFor(iface) : null;
    if (mock === null || !mock.methods.includes(method)) {
      return finish({});
    }
    if (mock.calls >= MOCK_LIMITS.maxCalls) {
      disable(mock, 'TESSERA_MOCK_CALL_LIMIT', 'The mock reached 1000 calls this session.');
      return finish({ state: 'error', response: { error: { code: 'TESSERA_MOCK_CALL_LIMIT', message: 'Call limit reached.' } } });
    }
    const occurrence = table.nextOccurrence(iface, method, argsKey);
    const started = timers.now();
    const callIndex = mock.calls;
    mock.calls += 1;
    const reply = await callWorker(mock, method, args, simTimeMs, callIndex);
    mock.cpuMs += timers.now() - started;
    const failure = (code, message) => {
      const response = { error: { code, message } };
      if (code === 'TESSERA_MOCK_TIMEOUT' || code === 'TESSERA_MOCK_SANDBOX_VIOLATION') {
        disable(mock, code, message);
      } else {
        mock.diagnostics.push(diag(code, message, { file: mock.path }));
      }
      return finish({ state: 'error', response });
    };
    if (reply.type === 'error') {
      return failure(reply.code, reply.message);
    }
    if (mock.cpuMs > MOCK_LIMITS.maxCpuMsPerSession) {
      disable(mock, 'TESSERA_MOCK_TIMEOUT', 'The mock used 10 s of CPU this session.');
    }
    if (reply.json.length > MOCK_LIMITS.maxResponseBytes * 4) {
      return failure('TESSERA_MOCK_RESPONSE_TOO_LARGE', 'The response is larger than 64 KiB.');
    }
    let parsed;
    try {
      parsed = JSON.parse(reply.json);
    } catch {
      return failure('TESSERA_MOCK_SYNTAX', 'The response is not JSON.');
    }
    const checked = validateResponse(parsed);
    if (!checked.ok) {
      return failure(checked.code, checked.message);
    }
    table.set(iface, method, argsKey, occurrence, checked.response);
    return finish({
      response: checked.response,
      state: 'error' in checked.response ? 'error' : 'default',
      delayMs: checked.delayMs,
    });
  }

  /**
   * Edits one traced call's response from the console's JSON text. Returns a diagnostic on a bad edit
   * and changes nothing; applying is explicit, never on keystroke (spec section 8.3).
   */
  function editResponse(seq, text) {
    const entry = trace.entries.find((candidate) => candidate.seq === seq);
    if (entry === undefined) {
      return diag('TESSERA_MOCK_SYNTAX', `No traced call ${seq}.`);
    }
    let parsed;
    try {
      parsed = JSON.parse(text);
    } catch (cause) {
      return diag('TESSERA_MOCK_SYNTAX', `The response is not valid JSON: ${cause.message}`);
    }
    const checked = validateResponse(parsed);
    if (!checked.ok) {
      return diag(checked.code, checked.message);
    }
    const argsKey = canonicalJson(entry.args);
    // The occurrence of this entry: how many earlier entries share its (interface, method, argsKey).
    const occurrence = trace.entries.filter(
      (other) =>
        other.seq < seq &&
        other.interface === entry.interface &&
        other.method === entry.method &&
        canonicalJson(other.args) === argsKey,
    ).length;
    table.set(entry.interface, entry.method, argsKey, occurrence, checked.response);
    entry.response = checked.response;
    entry.state = 'edited';
    entry.delayMs = checked.delayMs;
    emit();
    return null;
  }

  /** Clears the session: counters, the response table, the trace and every worker's session state. */
  function reset() {
    table.reset();
    trace = emptyTrace();
    trace.mockFiles = snapshotFiles();
    traceStopped = false;
    nextSeq = 0;
    for (const mock of mocks.values()) {
      mock.calls = 0;
      mock.cpuMs = 0;
      if (mock.worker !== null) {
        mock.worker.post({ type: 'reset' });
      }
    }
    emit();
  }

  /** Restarts a disabled mock's worker: the explicit console action (spec section 4.5). */
  async function restart(path) {
    const mock = mocks.get(path);
    if (mock === undefined || mock.source === '') {
      return;
    }
    stop(mock);
    mock.disabled = false;
    mock.diagnostics = mock.diagnostics.filter((d) => d.severity === 'Warn');
    mock.status = 'unbound';
    const started = await startWorker(mock);
    if (started.type === 'init-error') {
      disable(mock, started.code, started.message);
    } else {
      mock.interface = started.interface;
      mock.methods = started.methods;
      mock.calls = 0;
      mock.cpuMs = 0;
      bind(mock);
    }
    emit();
  }

  const snapshotFiles = () =>
    [...mocks.values()].map((m) => ({ path: m.path, version: m.version, sha256: m.sha256 }));

  return {
    load,
    call,
    editResponse,
    reset,
    restart,
    setEnabled(value) {
      enabled = value;
      emit();
    },
    clearTrace() {
      trace = emptyTrace();
      trace.mockFiles = snapshotFiles();
      traceStopped = false;
      emit();
    },
    get enabled() {
      return enabled;
    },
    get manifest() {
      return manifest;
    },
    /** Every mock's status for the console, in path order. */
    statuses() {
      return [...mocks.values()]
        .map(({ path, version, sha256, status, reason, interface: iface, methods, diagnostics, disabled, calls, cpuMs }) => ({
          path, version, sha256, status, reason, interface: iface, methods, diagnostics, disabled, calls, cpuMs,
        }))
        .sort((a, b) => (a.path < b.path ? -1 : 1));
    },
    /** The trace of this session (schema 1) with the mock files it was recorded against. */
    trace() {
      trace.mockFiles = snapshotFiles();
      return trace;
    },
    table,
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    /** Stops every worker. */
    dispose() {
      for (const mock of mocks.values()) {
        stop(mock);
      }
      listeners.clear();
    },
  };
}
