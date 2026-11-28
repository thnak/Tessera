// SPDX-License-Identifier: MIT

// The `format` service worker (ADR-061 slice 2, ADR-060, docs/specs/studio-services-v1.md section 8).
//
// Like the stub it is classic-script source, so one handle drives it in a page (blob Worker) and in Node
// (worker_threads). The worker half of the protocol (handshake, snapshot and delta cache, cancel,
// version checks) is the stub's; the one request is `format`. The formatter itself is an *engine*
// the source is built around, a function `tesseraFormat(text, printWidth, indentWidth, kind)` (kind: 0 .tsx, 1 .css, 2 tessera.json,
// absent means 0) that returns (or
// resolves to) the JSON of `format_edits_json`:
//   {"edits":[{offset,length,newText}],"diagnostics":[{code,message,line,column}],"skipped":[same shape]}
// `skipped` lists the regions of a .tsx file left as written because they do not parse (ADR-061 slice 4).
// In a page the engine is the Wasm module (`wasmEngineSource`); a gate may inject another.

const CORE = String.raw`
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

async function format(req) {
  const p = req.payload;
  if (!Number.isInteger(p.file) || p.file < 0) return fail(req, 'TESSERA_STUDIO_MALFORMED', 'format.file must be a file id');
  if (p.kind !== undefined && (!Number.isInteger(p.kind) || p.kind < 0 || p.kind > 2)) return fail(req, 'TESSERA_STUDIO_MALFORMED', 'format.kind must be 0, 1 or 2');
  if (!files.has(p.file)) return fail(req, 'TESSERA_FMT_NO_FILE', 'file ' + p.file + ' is not in the snapshot');
  let doc;
  try {
    doc = JSON.parse(await tesseraFormat(files.get(p.file), p.printWidth || 0, p.indentWidth || 0, p.kind || 0));
  } catch (e) {
    return fail(req, 'TESSERA_FMT_ENGINE', String((e && e.message) || e));
  }
  if (cancelled.delete(req.id)) return;
  // A refusal is an answer, not a failure: no edits, the diagnostics say why, the text is untouched.
  reply(req, {
    file: p.file,
    changed: doc.edits.length > 0,
    edits: doc.edits.map((e) => ({ file: p.file, offset: e.offset, length: e.length, newText: e.newText })),
    diagnostics: doc.diagnostics,
    skipped: doc.skipped || [],
  });
}

addEventListener('message', ({ data: req }) => {
  if (req.type === 'cancel') { cancelled.add(req.payload.target); return; }
  if (req.type === 'capabilities') {
    return reply(req, { name: 'format', protocol: PROTOCOL, features: ['format'] });
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
  if (req.type === 'format') { format(req); return; }
  fail(req, 'TESSERA_STUDIO_UNKNOWN_TYPE', req.type);
});
`;

/** The worker source around an engine (JS text that defines `tesseraFormat`). */
export function formatServiceSource(engineSource) {
  return `${engineSource}\n${CORE}`;
}

/**
 * The engine of a page: `tessera_format.wasm` through its ES6 loader (`createTesseraFormat`, built
 * by tools/fmt). A classic worker can `import()` a module, so the blob stays classic like the mock's.
 * The module is loaded once, on the first format, and each call frees its result with `tessera_free`.
 */
export function wasmEngineSource(moduleUrl) {
  return `
let tesseraModule = null;
async function tesseraFormat(text, printWidth, indentWidth, kind) {
  if (tesseraModule === null) {
    tesseraModule = import(${JSON.stringify(moduleUrl)}).then((m) => m.default());
  }
  const m = await tesseraModule;
  const size = m.lengthBytesUTF8(text) + 1;
  const input = m._malloc(size);
  m.stringToUTF8(text, input, size);
  let out = 0;
  try {
    out = m._tessera_format_edits_json_kind(kind || 0, input, printWidth, indentWidth);
    if (!out) throw new Error('the format module ran out of memory');
    return m.UTF8ToString(out);
  } finally {
    m._free(input);
    if (out) m._tessera_free(out);
  }
}`;
}
