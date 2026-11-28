// SPDX-License-Identifier: MIT

// The `analysis` service worker (#568, ADR-060, docs/specs/studio-services-v1.md sections 8 and 10).
//
// Like the format service it is classic-script source built around an *engine*, so one handle drives
// it in a page (blob Worker) and in Node (worker_threads). The worker half of the protocol
// (handshake, snapshot and delta, cancel, version checks) is the same; the requests are `symbols` and
// `diagnostics`. The engine is three functions the source defines, each returning (or resolving to)
// what the C++ library returns:
//   tesseraAnalysisSnapshot([{ file, path, text }])   replaces the project
//   tesseraAnalysisEdits([{ file, offset, length, newText }])   true when every edit applied
//   tesseraAnalysisQuery(type, file)   the JSON text of the result; file is -1 for every file
// In a page the engine is the Wasm module (`wasmEngineSource`, built from src/compiler/analysis); a gate
// may inject another. The parse runs in the worker, so typing on the page is never blocked by it.

const CORE = String.raw`
const PROTOCOL = 1;
let version = -1;
let haveSnapshot = false;
let ids = new Set();
const cancelled = new Set();
const decoder = new TextDecoder();
const reply = (req, result, error) =>
  postMessage({ v: PROTOCOL, kind: 'response', service: req.service, type: req.type, id: req.id, version: req.version,
    payload: error ? { error } : { result } });
const fail = (req, code, message) => reply(req, null, { code, message });

async function query(req) {
  const file = req.payload.file;
  if (file !== undefined && (!Number.isInteger(file) || file < 0)) return fail(req, 'TESSERA_STUDIO_MALFORMED', req.type + '.file must be a file id');
  if (file !== undefined && !ids.has(file)) return fail(req, 'TESSERA_ANALYSIS_NO_FILE', 'file ' + file + ' is not in the snapshot');
  let result;
  try {
    result = JSON.parse(await tesseraAnalysisQuery(req.type, file === undefined ? -1 : file));
  } catch (e) {
    return fail(req, 'TESSERA_ANALYSIS_ENGINE', String((e && e.message) || e));
  }
  if (cancelled.delete(req.id)) return;
  reply(req, result);
}

async function mutate(req) {
  try {
    if (req.type === 'snapshot') {
      const files = req.payload.files.map((f) => ({ file: f.file, path: typeof f.path === 'string' ? f.path : '', text: decoder.decode(f.bytes) }));
      await tesseraAnalysisSnapshot(files);
      ids = new Set(files.map((f) => f.file));
      haveSnapshot = true;
      version = req.version;
      return reply(req, { files: files.length });
    }
    if (!haveSnapshot) return fail(req, 'TESSERA_STUDIO_NO_SNAPSHOT', 'no snapshot yet');
    if (req.payload.baseVersion !== version) return fail(req, 'TESSERA_STUDIO_VERSION', 'delta base ' + req.payload.baseVersion + ' != ' + version);
    if (!req.payload.edits.every((e) => ids.has(e.file)) || !(await tesseraAnalysisEdits(req.payload.edits))) {
      return fail(req, 'TESSERA_STUDIO_VERSION', 'an edit does not apply to the cache; send a snapshot');
    }
    version = req.version;
    return reply(req, { version });
  } catch (e) {
    return fail(req, 'TESSERA_ANALYSIS_ENGINE', String((e && e.message) || e));
  }
}

// Requests that change the project run one after another, and a query waits for them, so an answer
// is always computed against the version it carries.
let chain = Promise.resolve();

addEventListener('message', ({ data: req }) => {
  if (req.type === 'cancel') { cancelled.add(req.payload.target); return; }
  if (req.type === 'capabilities') {
    return reply(req, { name: 'analysis', protocol: PROTOCOL, features: ['symbols', 'diagnostics'] });
  }
  if (req.type === 'snapshot' || req.type === 'delta') {
    chain = chain.then(() => mutate(req));
    return;
  }
  chain = chain.then(async () => {
    if (!haveSnapshot) return fail(req, 'TESSERA_STUDIO_NO_SNAPSHOT', 'no snapshot yet');
    if (req.version > version) return fail(req, 'TESSERA_STUDIO_VERSION', 'request is newer than the cache');
    if (req.type === 'symbols' || req.type === 'diagnostics') return query(req);
    fail(req, 'TESSERA_STUDIO_UNKNOWN_TYPE', req.type);
  });
});
`;

/** The worker source around an engine (JS text that defines the three `tesseraAnalysis*` functions). */
export function analysisServiceSource(engineSource) {
  return `${engineSource}\n${CORE}`;
}

/**
 * The engine of a page: `tessera_analysis.wasm` through its ES6 loader (`createTesseraAnalysis`, built
 * from src/compiler/analysis). A classic worker can `import()` a module, so the blob stays classic. The
 * module is loaded once, on the first request, and each returned document is freed with `tessera_free`.
 */
export function wasmEngineSource(moduleUrl) {
  return `
let tesseraModule = null;
const tesseraLoad = () => {
  if (tesseraModule === null) tesseraModule = import(${JSON.stringify(moduleUrl)}).then((m) => m.default());
  return tesseraModule;
};
function tesseraCString(m, text) {
  const size = m.lengthBytesUTF8(text) + 1;
  const ptr = m._malloc(size);
  m.stringToUTF8(text, ptr, size);
  return ptr;
}
async function tesseraAnalysisSnapshot(files) {
  const m = await tesseraLoad();
  m._tessera_analysis_reset();
  for (const f of files) {
    const path = tesseraCString(m, f.path);
    const text = tesseraCString(m, f.text);
    try { m._tessera_analysis_set_file(f.file, path, text); } finally { m._free(path); m._free(text); }
  }
  m._tessera_analysis_commit_snapshot();
}
async function tesseraAnalysisEdits(edits) {
  const m = await tesseraLoad();
  for (const e of edits) {
    const text = tesseraCString(m, e.newText);
    try { m._tessera_analysis_edit(e.file, e.offset, e.length, text); } finally { m._free(text); }
  }
  return m._tessera_analysis_commit_edits() === 1;
}
async function tesseraAnalysisQuery(type, file) {
  const m = await tesseraLoad();
  const kind = tesseraCString(m, type);
  let out = 0;
  try {
    out = m._tessera_analysis_query_json(kind, file);
    if (!out) throw new Error('the analysis module could not answer ' + type);
    return m.UTF8ToString(out);
  } finally {
    m._free(kind);
    if (out) m._tessera_free(out);
  }
}`;
}
