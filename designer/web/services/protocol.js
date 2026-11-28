// SPDX-License-Identifier: MIT

// The studio service protocol (docs/specs/studio-services-v1.md): constants and the schema check.
//
// No DOM, React or CodeMirror here: the same module runs in a page, a worker and Node, and the
// Tessera-built studio reuses it unchanged (ADR-060).

export const PROTOCOL_VERSION = 1;
export const KINDS = ['request', 'response', 'event'];
export const MAX_DEPTH = 32;

export const ERROR_CODES = {
  malformed: 'TESSERA_STUDIO_MALFORMED',
  version: 'TESSERA_STUDIO_VERSION',
  noSnapshot: 'TESSERA_STUDIO_NO_SNAPSHOT',
  unknownType: 'TESSERA_STUDIO_UNKNOWN_TYPE',
  cancelled: 'TESSERA_STUDIO_CANCELLED',
  stale: 'TESSERA_STUDIO_STALE',
};

/**
 * Why `value` is not plain data, or null when it is. Plain data is what structured clone carries
 * without surprises: null, boolean, string, finite number, array, plain object, ArrayBuffer and
 * typed-array views. A function, symbol, bigint, undefined, class instance (a DOM node, a Map) or a
 * cycle is refused.
 * @param {unknown} value
 * @param {string} [at]
 * @returns {string | null}
 */
export function plainDataProblem(value, at = '$', seen = new WeakSet(), depth = 0) {
  if (depth > MAX_DEPTH) return `${at}: nested deeper than ${MAX_DEPTH}`;
  if (value === null) return null;
  switch (typeof value) {
    case 'boolean':
    case 'string':
      return null;
    case 'number':
      return Number.isFinite(value) ? null : `${at}: non-finite number`;
    case 'object':
      break;
    default:
      return `${at}: ${typeof value} is not plain data`;
  }
  if (value instanceof ArrayBuffer || ArrayBuffer.isView(value)) return null;
  if (seen.has(value)) return `${at}: cycle`;
  seen.add(value);
  if (Array.isArray(value)) {
    for (let i = 0; i < value.length; i += 1) {
      const p = plainDataProblem(value[i], `${at}[${i}]`, seen, depth + 1);
      if (p) return p;
    }
    return null;
  }
  const proto = Object.getPrototypeOf(value);
  if (proto !== Object.prototype && proto !== null) return `${at}: ${proto.constructor?.name ?? 'object'} is not plain data`;
  for (const key of Object.keys(value)) {
    const p = plainDataProblem(value[key], `${at}.${key}`, seen, depth + 1);
    if (p) return p;
  }
  return null;
}

const isUint = (n) => Number.isInteger(n) && n >= 0;

/**
 * Checks one message against the envelope of the spec, section 3, and returns a list of problems
 * (empty when valid).
 * @param {unknown} m
 * @returns {string[]}
 */
export function validateMessage(m) {
  if (m === null || typeof m !== 'object' || Array.isArray(m)) return ['message is not an object'];
  const plain = plainDataProblem(m);
  if (plain) return [plain];
  const problems = [];
  if (m.v !== PROTOCOL_VERSION) problems.push(`v must be ${PROTOCOL_VERSION}`);
  if (!KINDS.includes(m.kind)) problems.push('kind must be request, response or event');
  if (typeof m.service !== 'string' || m.service === '') problems.push('service must be a non-empty string');
  if (typeof m.type !== 'string' || m.type === '') problems.push('type must be a non-empty string');
  if (m.kind !== 'event' && !isUint(m.id)) problems.push('id must be a non-negative integer');
  if (m.kind !== 'event' && !isUint(m.version)) problems.push('version must be a non-negative integer');
  if (m.payload === null || typeof m.payload !== 'object' || Array.isArray(m.payload)) problems.push('payload must be an object');
  if (problems.length > 0) return problems;
  if (m.kind === 'request') problems.push(...payloadProblems(m.type, m.payload));
  return problems;
}

function payloadProblems(type, p) {
  const out = [];
  if (type === 'snapshot') {
    if (!Array.isArray(p.files)) return ['snapshot.files must be an array'];
    for (const f of p.files) {
      if (!isUint(f?.file)) out.push('snapshot file id must be a non-negative integer');
      if (!(f?.bytes instanceof ArrayBuffer)) out.push('snapshot bytes must be an ArrayBuffer');
    }
  } else if (type === 'delta') {
    if (!isUint(p.baseVersion)) out.push('delta.baseVersion must be a non-negative integer');
    if (!Array.isArray(p.edits)) return [...out, 'delta.edits must be an array'];
    for (const e of p.edits) {
      if (!isUint(e?.file) || !isUint(e?.offset) || !isUint(e?.length) || typeof e?.newText !== 'string') {
        out.push('delta edit needs file, offset, length (UTF-16) and newText');
      }
    }
  } else if (type === 'cancel') {
    if (!isUint(p.target)) out.push('cancel.target must be a request id');
  }
  return out;
}
