// SPDX-License-Identifier: MIT

// The `compile` service worker (#569, ADR-060, docs/specs/studio-services-v1.md section 14): the page's
// compile-and-diagnose view behind the services protocol, so the page never calls a Wasm export itself.
//
// Like the format and analysis services it is classic-script source built around an *engine*, so one
// handle drives it in a page (blob Worker) and in Node (worker_threads). The worker half of the protocol
// (handshake, snapshot and delta cache, cancel, version checks) is the same; the one request is
// `diagnostics`. The engine is one function the source defines, returning (or resolving to) the JSON text
// of the compiler's diagnostics document, the one `tessera_diagnostics_json` answers:
//   tesseraCompile({ entry, sources: [{ path, text }], stylesheets: [{ path, text }], servicesManifest })
// In a page the engine is the existing `tessera_diagnostics` Wasm module (`wasmEngineSource`); a gate may
// inject another. The compile runs in the worker, so typing on the page is never blocked by it.

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

async function diagnostics(req) {
  const p = req.payload;
  if (p.entry !== undefined && p.entry !== null && typeof p.entry !== 'string') return fail(req, 'TESSERA_STUDIO_MALFORMED', 'diagnostics.entry must be a path or null');
  if (p.servicesManifest !== undefined && p.servicesManifest !== null && typeof p.servicesManifest !== 'string') return fail(req, 'TESSERA_STUDIO_MALFORMED', 'diagnostics.servicesManifest must be text or null');
  const sources = [];
  const stylesheets = [];
  for (const id of [...files.keys()].sort((a, b) => a - b)) {
    const path = paths.get(id) ?? '';
    (path.endsWith('.css') ? stylesheets : sources).push({ path, text: files.get(id) });
  }
  let document;
  try {
    document = JSON.parse(await tesseraCompile({ entry: p.entry ?? null, sources, stylesheets, servicesManifest: p.servicesManifest ?? null }));
  } catch (e) {
    return fail(req, 'TESSERA_COMPILE_ENGINE', String((e && e.message) || e));
  }
  if (cancelled.delete(req.id)) return;
  reply(req, { document });
}

addEventListener('message', ({ data: req }) => {
  if (req.type === 'cancel') { cancelled.add(req.payload.target); return; }
  if (req.type === 'capabilities') {
    return reply(req, { name: 'compile', protocol: PROTOCOL, features: ['diagnostics'] });
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
  if (req.type === 'diagnostics') { diagnostics(req); return; }
  fail(req, 'TESSERA_STUDIO_UNKNOWN_TYPE', req.type);
});
`;

/** The worker source around an engine (JS text that defines `tesseraCompile`). */
export function compileServiceSource(engineSource) {
  return `${engineSource}\n${CORE}`;
}

/**
 * The engine of a page: `tessera_diagnostics.wasm` through its ES6 loader. The project is staged with the
 * `tessera_project_*` exports (what `lib/wasm-call.ts` did on the page thread) and the document is freed with
 * `tessera_free`. The module is loaded once, on the first request.
 */
export function wasmEngineSource(moduleUrl) {
  return `
let tesseraModule = null;
function tesseraCString(m, text) {
  const size = m.lengthBytesUTF8(text) + 1;
  const ptr = m._malloc(size);
  m.stringToUTF8(text, ptr, size);
  return ptr;
}
function tesseraStage(m, name, strings) {
  const pointers = strings.map((s) => tesseraCString(m, s));
  try {
    if (typeof m[name] !== 'function') throw new Error('the module does not export ' + name);
    m[name](...pointers);
  } finally {
    for (const p of pointers) m._free(p);
  }
}
async function tesseraCompile(project) {
  if (tesseraModule === null) tesseraModule = import(${JSON.stringify(moduleUrl)}).then((m) => m.default());
  const m = await tesseraModule;
  tesseraStage(m, '_tessera_project_begin', []);
  for (const s of project.sources) tesseraStage(m, '_tessera_project_add_source', [s.path, s.text]);
  for (const s of project.stylesheets) tesseraStage(m, '_tessera_project_add_stylesheet', [s.path, s.text]);
  if (project.servicesManifest) tesseraStage(m, '_tessera_project_set_services', [project.servicesManifest]);
  if (project.entry !== null) tesseraStage(m, '_tessera_project_set_entry', [project.entry]);
  if (typeof m._tessera_diagnostics_json !== 'function') throw new Error('the module does not export _tessera_diagnostics_json');
  const out = m._tessera_diagnostics_json();
  if (!out) throw new Error('_tessera_diagnostics_json returned no document');
  try { return m.UTF8ToString(out); } finally { m._tessera_free(out); }
}`;
}
