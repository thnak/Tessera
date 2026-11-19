// SPDX-License-Identifier: MIT

// The call trace (docs/specs/studio-mock-services-v1.md section 7): versioned JSON, schema 1.
//
// A trace names the exact mock files it was recorded with (path, version, sha256), so a replay against
// different digests can warn. Replaying is the deterministic mode: `fillTable` turns a trace back into
// the response table, and `ReplayCursor` checks each later call against the next entry so a diverging
// call is `TESSERA_MOCK_REPLAY_MISMATCH` instead of a silently different frame.
//
// The reader rejects an unknown schema version rather than guessing (section 13).

import { MOCK_LIMITS, byteLength, canonicalJson, validateResponse } from './response.js';

export const TRACE_SCHEMA = 1;

/** A new, empty trace. `stopped` is set to a code when the trace limit ended recording. */
export function emptyTrace() {
  return { schema: TRACE_SCHEMA, mockFiles: [], entries: [], stopped: null };
}

/** The trace as the downloadable JSON object: no console-only fields. */
export function toDownload(trace) {
  return {
    schema: trace.schema,
    mockFiles: trace.mockFiles,
    entries: trace.entries.map(({ seq, interface: iface, method, args, response, simTimeMs, state }) => ({
      seq,
      interface: iface,
      method,
      args,
      response,
      simTimeMs,
      ...(state === 'unmocked' ? { unmocked: true } : {}),
    })),
  };
}

/** Bytes of the canonical JSON of the trace, for the 4 MiB cap. */
export function traceBytes(trace) {
  try {
    return byteLength(canonicalJson(toDownload(trace)));
  } catch {
    return Infinity;
  }
}

/**
 * Parses and validates a trace document. Throws `Error` with a `code` on a violation.
 *
 * @param {unknown} value a parsed JSON value
 */
export function readTrace(value) {
  const bad = (code, message) => Object.assign(new Error(message), { code });
  if (value === null || typeof value !== 'object' || value.schema !== TRACE_SCHEMA) {
    throw bad('TESSERA_MOCK_SYNTAX', 'Unknown trace schema; expected schema 1.');
  }
  if (!Array.isArray(value.entries) || !Array.isArray(value.mockFiles)) {
    throw bad('TESSERA_MOCK_SYNTAX', 'A trace has `mockFiles` and `entries` arrays.');
  }
  if (value.entries.length > MOCK_LIMITS.maxTraceEntries) {
    throw bad('TESSERA_MOCK_TRACE_LIMIT', 'A trace has at most 10000 entries.');
  }
  for (const [index, entry] of value.entries.entries()) {
    const ok =
      entry !== null &&
      typeof entry === 'object' &&
      entry.seq === index &&
      typeof entry.interface === 'string' &&
      typeof entry.method === 'string' &&
      Array.isArray(entry.args) &&
      Number.isInteger(entry.simTimeMs);
    if (!ok) {
      throw bad('TESSERA_MOCK_SYNTAX', `Trace entry ${index} is malformed (seq must be 0-based and in order).`);
    }
    if (entry.response !== null && entry.response !== undefined && !validateResponse(entry.response).ok) {
      throw bad('TESSERA_MOCK_SYNTAX', `Trace entry ${index} has an invalid response.`);
    }
  }
  return value;
}

/** Fills `table` from a trace's mocked entries, keyed by (interface, method, argsKey, occurrence). */
export function fillTable(table, trace) {
  for (const entry of trace.entries) {
    if (entry.response) {
      const argsKey = canonicalJson(entry.args);
      const occurrence = table.nextOccurrence(entry.interface, entry.method, argsKey);
      table.set(entry.interface, entry.method, argsKey, occurrence, entry.response);
    }
  }
}

/** Compares recorded files with the current ones; returns the paths whose digest differs. */
export function digestMismatches(trace, currentFiles) {
  const now = new Map(currentFiles.map((f) => [f.path, f.sha256]));
  return trace.mockFiles.filter((f) => now.get(f.path) !== f.sha256).map((f) => f.path);
}

/** Walks a trace as calls arrive; the first call that does not match the next entry stops the replay. */
export class ReplayCursor {
  constructor(trace) {
    this.trace = trace;
    this.index = 0;
    this.stopped = false;
  }

  /** `null` when the call matches, else the `TESSERA_MOCK_REPLAY_MISMATCH` message (and the replay stops). */
  next(iface, method, args) {
    if (this.stopped) {
      return 'The replay already stopped.';
    }
    const expected = this.trace.entries[this.index];
    if (
      expected === undefined ||
      expected.interface !== iface ||
      expected.method !== method ||
      canonicalJson(expected.args) !== canonicalJson(args)
    ) {
      this.stopped = true;
      return `Call ${this.index} (${iface}.${method}) does not match the trace.`;
    }
    this.index += 1;
    return null;
  }
}
