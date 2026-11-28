// SPDX-License-Identifier: MIT

// The `search` service worker (#580, ADR-060, docs/specs/studio-services-v1.md section 15).
//
// Classic-script source like the stub and the format worker, so one handle drives it in a page (blob Worker)
// and in Node (worker_threads). The protocol half (handshake, snapshot and delta cache, cancel, version
// checks) is the stub's; the two requests are `search` and `replace`, both over the cached snapshot with plain
// data in and out. There is no WebAssembly engine: the matcher is the JS text of `search-engine.js` and the
// bounded regex VM of `search-regex.js`, which the gate evaluates unchanged.

import { searchEngineSource } from './search-engine.js';
import { searchRegexSource } from './search-regex.js';

const CORE = String.raw`
const PROTOCOL = 1;
let version = -1;
let haveSnapshot = false;
const files = new Map();
const paths = new Map();
const cancelled = new Set();
const decoder = new TextDecoder();
const reply = (req, result, error) =>
  postMessage({ v: PROTOCOL, kind: 'response', service: req.service, type: req.type, id: req.id, version: req.version,
    payload: error ? { error } : { result } });
const fail = (req, code, message) => reply(req, null, { code, message });

function options(p) {
  const text = (value) => (typeof value === 'string' ? value : '');
  return {
    query: p.query, caseSensitive: p.caseSensitive === true, wholeWord: p.wholeWord === true, regex: p.regex === true,
    include: text(p.include), exclude: text(p.exclude), maxResults: p.maxResults,
  };
}
const snapshotFiles = () => [...files].map(([file, text]) => ({ file, path: paths.get(file) ?? ('#' + file), text }));

async function run(req) {
  const p = req.payload;
  if (typeof p.query !== 'string') return fail(req, 'TESSERA_STUDIO_MALFORMED', req.type + '.query must be a string');
  if (req.type === 'replace' && typeof p.replacement !== 'string') return fail(req, 'TESSERA_STUDIO_MALFORMED', 'replace.replacement must be a string');
  if (p.only !== undefined && p.only !== null && !(Array.isArray(p.only) && p.only.every((pick) => pick && Number.isInteger(pick.file) && Number.isInteger(pick.offset)))) {
    return fail(req, 'TESSERA_STUDIO_MALFORMED', 'replace.only must be a list of { file, offset }');
  }
  const stop = () => cancelled.has(req.id);
  let result;
  try {
    result = req.type === 'search'
      ? await tesseraSearch(snapshotFiles(), options(p), stop)
      : await tesseraReplace(snapshotFiles(), options(p), p.replacement, p.only ?? null, stop);
  } catch (e) {
    return fail(req, e && e.code ? e.code : 'TESSERA_SEARCH_ENGINE', String((e && e.message) || e));
  }
  if (cancelled.delete(req.id)) return;
  reply(req, result);
}

addEventListener('message', ({ data: req }) => {
  if (req.type === 'cancel') { cancelled.add(req.payload.target); return; }
  if (req.type === 'capabilities') {
    return reply(req, { name: 'search', protocol: PROTOCOL, features: ['search', 'replace'] });
  }
  if (req.type === 'snapshot') {
    files.clear();
    paths.clear();
    for (const f of req.payload.files) {
      files.set(f.file, decoder.decode(f.bytes));
      if (typeof f.path === 'string') paths.set(f.file, f.path);
    }
    version = req.version; haveSnapshot = true;
    return reply(req, { files: files.size });
  }
  if (!haveSnapshot) return fail(req, 'TESSERA_STUDIO_NO_SNAPSHOT', 'no snapshot yet');
  if (req.type === 'delta') {
    if (req.payload.baseVersion !== version) return fail(req, 'TESSERA_STUDIO_VERSION', 'delta base ' + req.payload.baseVersion + ' != ' + version);
    if (!req.payload.edits.every((e) => files.has(e.file))) return fail(req, 'TESSERA_STUDIO_VERSION', 'an edit names a file the cache does not hold; send a snapshot');
    for (const e of req.payload.edits) {
      const t = files.get(e.file);
      files.set(e.file, t.slice(0, e.offset) + e.newText + t.slice(e.offset + e.length));
    }
    version = req.version;
    return reply(req, { version });
  }
  if (req.version > version) return fail(req, 'TESSERA_STUDIO_VERSION', 'request is newer than the cache');
  if (req.type === 'search' || req.type === 'replace') { run(req); return; }
  fail(req, 'TESSERA_STUDIO_UNKNOWN_TYPE', req.type);
});
`;

/** The worker source: the regex VM, the matcher, then the protocol shell. */
export function searchServiceSource() {
  return searchRegexSource + '\n' + searchEngineSource + '\n' + CORE;
}
