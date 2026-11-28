// SPDX-License-Identifier: MIT

// A stub service worker, as classic-script source (the same shape the mock worker uses), so one
// handle interface drives it in a page (blob Worker) and in Node (worker_threads). It implements the
// worker half of the protocol: handshake, snapshot and delta cache, cancel, version checks. The real
// services (#568 and after) replace its two sample requests, not the rest.

export const STUB_SERVICE_SOURCE = String.raw`
const PROTOCOL = 1;
let version = -1;
let haveSnapshot = false;
const files = new Map();
const cancelled = new Set();
const decoder = new TextDecoder();
const reply = (req, result, error) =>
  postMessage({ v: PROTOCOL, kind: 'response', service: req.service, type: req.type, id: req.id, version: req.version,
    payload: error ? { error } : { result } });
const fail = (req, code, message) => reply(req, null, { code, message });

addEventListener('message', ({ data: req }) => {
  if (req.type === 'cancel') { cancelled.add(req.payload.target); return; }
  if (req.type === 'capabilities') {
    return reply(req, { name: 'stub', protocol: PROTOCOL, features: ['echo', 'slow'] });
  }
  if (req.type === 'snapshot') {
    files.clear();
    for (const f of req.payload.files) files.set(f.file, decoder.decode(f.bytes));
    version = req.version; haveSnapshot = true;
    return reply(req, { files: files.size });
  }
  if (!haveSnapshot) return fail(req, 'TESSERA_STUDIO_NO_SNAPSHOT', 'no snapshot yet');
  if (req.type === 'delta') {
    if (req.payload.baseVersion !== version) return fail(req, 'TESSERA_STUDIO_VERSION', 'delta base ' + req.payload.baseVersion + ' != ' + version);
    for (const e of req.payload.edits) {
      const t = files.get(e.file) || '';
      files.set(e.file, t.slice(0, e.offset) + e.newText + t.slice(e.offset + e.length));
    }
    version = req.version;
    return reply(req, { version });
  }
  if (req.version > version) return fail(req, 'TESSERA_STUDIO_VERSION', 'request is newer than the cache');
  if (req.type === 'echo') return reply(req, { length: (files.get(req.payload.file) || '').length });
  if (req.type === 'slow') {
    setTimeout(() => { if (!cancelled.has(req.id)) reply(req, { done: true }); }, req.payload.delayMs);
    return;
  }
  fail(req, 'TESSERA_STUDIO_UNKNOWN_TYPE', req.type);
});
`;
