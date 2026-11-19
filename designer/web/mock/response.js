// SPDX-License-Identifier: MIT

// Mock responses, canonical JSON and the response table (docs/specs/studio-mock-services-v1.md
// sections 3.2, 6.3 and 6.6, limits in section 12).
//
// **The host never trusts the worker.** A response is data from code the host did not write, so its
// shape, JSON validity and size are checked here before anything uses or records it. A violation is a
// named code, never a silent truncation (ADR-055 decision 2).
//
// **Canonical JSON** (sorted keys, no whitespace) is the identity of a call's arguments: the response
// table is keyed by `(interface, method, argsKey, occurrence)`, so two identical calls in one session
// get identical responses and the viewer can verify the key the host computed.

export const MOCK_LIMITS = Object.freeze({
  maxCalls: 1000,
  maxResponseBytes: 64 * 1024,
  maxArgsBytes: 8 * 1024,
  maxEntriesPerBatch: 256,
  maxErrorMessageBytes: 1024,
  maxDelayMs: 60000,
  maxCpuMsPerSession: 10000,
  maxCpuMsPerCall: 50,
  maxTraceEntries: 10000,
  maxTraceBytes: 4 * 1024 * 1024,
});

const encoder = new TextEncoder();

/** UTF-8 length of `text`. */
export function byteLength(text) {
  return encoder.encode(text).length;
}

/**
 * Canonical JSON: object keys sorted, no whitespace. Throws `TypeError` for a value JSON cannot hold
 * (`undefined`, a function, a cycle, a non-finite number), which the callers turn into a named error.
 *
 * @param {unknown} value
 * @param {Set<object>} [seen]
 * @returns {string}
 */
export function canonicalJson(value, seen = new Set()) {
  if (value === null) {
    return 'null';
  }
  switch (typeof value) {
    case 'string':
    case 'boolean':
      return JSON.stringify(value);
    case 'number':
      if (!Number.isFinite(value)) {
        throw new TypeError('a non-finite number is not JSON');
      }
      return JSON.stringify(value);
    case 'object': {
      if (seen.has(value)) {
        throw new TypeError('a cycle is not JSON');
      }
      seen.add(value);
      let out;
      if (Array.isArray(value)) {
        out = `[${value.map((item) => canonicalJson(item, seen)).join(',')}]`;
      } else {
        const proto = Object.getPrototypeOf(value);
        if (proto !== Object.prototype && proto !== null) {
          throw new TypeError('only plain objects are JSON');
        }
        const keys = Object.keys(value).sort();
        out = `{${keys.map((key) => `${JSON.stringify(key)}:${canonicalJson(value[key], seen)}`).join(',')}}`;
      }
      seen.delete(value);
      return out;
    }
    default:
      throw new TypeError(`a ${typeof value} is not JSON`);
  }
}

/** The key for a call's arguments, or `null` when they are not JSON or exceed 8 KiB. */
export function argsKeyOf(args) {
  try {
    const key = canonicalJson(args);
    return byteLength(key) <= MOCK_LIMITS.maxArgsBytes ? key : null;
  } catch {
    return null;
  }
}

const ERROR_CODE = /^[A-Z0-9_]{1,64}$/;

/**
 * Validates one `MockResult` (section 3.2) returned by a handler.
 *
 * @param {unknown} result
 * @returns {{ ok: true, response: object, delayMs: number | null } | { ok: false, code: string, message: string }}
 */
export function validateResponse(result) {
  const bad = (code, message) => ({ ok: false, code, message });
  if (result === null || typeof result !== 'object' || Array.isArray(result)) {
    return bad('TESSERA_MOCK_SYNTAX', 'A handler must return { value } or { error }.');
  }
  const keys = Object.keys(result);
  const hasValue = keys.includes('value');
  const hasError = keys.includes('error');
  if (hasValue === hasError) {
    return bad('TESSERA_MOCK_SYNTAX', 'A response has exactly one of `value` and `error`.');
  }
  for (const key of keys) {
    if (key !== 'value' && key !== 'error' && key !== 'delayMs') {
      return bad('TESSERA_MOCK_SYNTAX', `A response has an unknown field \`${key}\`.`);
    }
  }
  let delayMs = null;
  if (result.delayMs !== undefined) {
    const d = result.delayMs;
    if (!Number.isInteger(d) || d < 0 || d > MOCK_LIMITS.maxDelayMs) {
      return bad('TESSERA_MOCK_SYNTAX', '`delayMs` must be an integer from 0 to 60000.');
    }
    delayMs = d;
  }
  if (hasError) {
    const error = result.error;
    if (
      error === null ||
      typeof error !== 'object' ||
      typeof error.code !== 'string' ||
      !ERROR_CODE.test(error.code) ||
      typeof error.message !== 'string' ||
      byteLength(error.message) > MOCK_LIMITS.maxErrorMessageBytes
    ) {
      return bad(
        'TESSERA_MOCK_SYNTAX',
        'An error response is { code: [A-Z0-9_]{1,64}, message: at most 1 KiB }.',
      );
    }
    return { ok: true, response: { error: { code: error.code, message: error.message } }, delayMs };
  }
  let json;
  try {
    json = canonicalJson(result.value);
  } catch (cause) {
    return bad('TESSERA_MOCK_SYNTAX', `The response value is not JSON: ${cause.message}.`);
  }
  if (byteLength(json) > MOCK_LIMITS.maxResponseBytes) {
    return bad('TESSERA_MOCK_RESPONSE_TOO_LARGE', 'The response is larger than 64 KiB of canonical JSON.');
  }
  return { ok: true, response: { value: JSON.parse(json) }, delayMs };
}

/**
 * The response table of section 6: `(interface, method, argsKey, occurrence) -> response`.
 *
 * The host fills it; `entries()` is what a `mock.responses` message carries. `occurrenceOf` counts the
 * repetitions of one argsKey so "the third identical call" has its own entry.
 */
export class ResponseTable {
  constructor() {
    /** @type {Map<string, object>} */
    this.map = new Map();
    /** @type {Map<string, number>} */
    this.counts = new Map();
  }

  static #key(iface, method, argsKey, occurrence) {
    return `${iface}\u0000${method}\u0000${argsKey}\u0000${occurrence}`;
  }

  /** The next occurrence index for this call, and advance it. */
  nextOccurrence(iface, method, argsKey) {
    const id = `${iface}\u0000${method}\u0000${argsKey}`;
    const occurrence = this.counts.get(id) ?? 0;
    this.counts.set(id, occurrence + 1);
    return occurrence;
  }

  set(iface, method, argsKey, occurrence, response) {
    this.map.set(ResponseTable.#key(iface, method, argsKey, occurrence), response);
  }

  get(iface, method, argsKey, occurrence) {
    return this.map.get(ResponseTable.#key(iface, method, argsKey, occurrence));
  }

  /** Clears entries and counters (`mock.reset`). */
  reset() {
    this.map.clear();
    this.counts.clear();
  }

  /** The entries as a `mock.responses` payload, split into batches of at most 256. */
  batches() {
    const all = [];
    for (const [key, response] of this.map) {
      const [iface, method, argsKey, occurrence] = key.split('\u0000');
      all.push({ interface: iface, method, argsKey, occurrence: Number(occurrence), response });
    }
    const out = [];
    for (let k = 0; k < all.length; k += MOCK_LIMITS.maxEntriesPerBatch) {
      out.push(all.slice(k, k + MOCK_LIMITS.maxEntriesPerBatch));
    }
    return out;
  }
}
