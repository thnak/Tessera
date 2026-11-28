// SPDX-License-Identifier: MIT

// What the studio can say about a project asset without a browser (issue #581): kind and format, image
// dimensions, a font's family and license strings, a size-budget hint, and the references to an asset in
// source and CSS, with the plan for renaming or replacing it.
//
// Everything is pure data in and out (bytes are a Uint8Array, files are { path, text }), with no imports and
// no DOM, so a Node gate drives what the page ships. The asset editor panel (preview, drag-and-drop replace)
// is the browser half and lives on top of this module.

/** Extension to the asset class the panel draws. */
const IMAGE_EXT = new Set(['png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'ico', 'svg']);
const FONT_EXT = new Set(['ttf', 'otf', 'woff', 'woff2', 'ttc']);

/** Extension of a path, lower case, without the dot ('' for none). */
function extOf(path) {
  const base = path.slice(path.lastIndexOf('/') + 1);
  const dot = base.lastIndexOf('.');
  return dot <= 0 ? '' : base.slice(dot + 1).toLowerCase();
}

/** 'image' | 'font' | 'other' by extension (an SVG is an image; its symbols are listed by svgSymbols). */
export function assetClass(path) {
  const ext = extOf(path);
  if (IMAGE_EXT.has(ext)) return 'image';
  if (FONT_EXT.has(ext)) return 'font';
  return 'other';
}

const u16be = (b, o) => (b[o] << 8) | b[o + 1];
const u32be = (b, o) => ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0;
const u16le = (b, o) => b[o] | (b[o + 1] << 8);
const u24le = (b, o) => b[o] | (b[o + 1] << 8) | (b[o + 2] << 16);
const u32le = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const ascii = (b, o, n) => String.fromCharCode(...b.subarray(o, o + n));

function pngInfo(b) {
  if (b.length < 24 || u32be(b, 0) !== 0x89504e47 || ascii(b, 12, 4) !== 'IHDR') return null;
  return { format: 'png', width: u32be(b, 16), height: u32be(b, 20) };
}

function jpegInfo(b) {
  if (b.length < 4 || b[0] !== 0xff || b[1] !== 0xd8) return null;
  let o = 2;
  while (o + 4 <= b.length) {
    if (b[o] !== 0xff) return null;
    const marker = b[o + 1];
    if (marker === 0xff) { o += 1; continue; }
    // Standalone markers carry no length.
    if (marker === 0xd8 || marker === 0x01 || (marker >= 0xd0 && marker <= 0xd7)) { o += 2; continue; }
    const length = u16be(b, o + 2);
    const isSof = marker >= 0xc0 && marker <= 0xcf && marker !== 0xc4 && marker !== 0xc8 && marker !== 0xcc;
    if (isSof) {
      if (o + 9 > b.length) return null;
      return { format: 'jpeg', height: u16be(b, o + 5), width: u16be(b, o + 7) };
    }
    if (length < 2) return null;
    o += 2 + length;
  }
  return null;
}

function gifInfo(b) {
  const sig = b.length >= 10 ? ascii(b, 0, 6) : '';
  if (sig !== 'GIF87a' && sig !== 'GIF89a') return null;
  return { format: 'gif', width: u16le(b, 6), height: u16le(b, 8) };
}

function webpInfo(b) {
  if (b.length < 25 || ascii(b, 0, 4) !== 'RIFF' || ascii(b, 8, 4) !== 'WEBP') return null;
  const chunk = ascii(b, 12, 4);
  if (chunk === 'VP8X' && b.length >= 30) return { format: 'webp', width: u24le(b, 24) + 1, height: u24le(b, 27) + 1 };
  if (chunk === 'VP8L' && b[20] === 0x2f) {
    const bits = u32le(b, 21);
    return { format: 'webp', width: (bits & 0x3fff) + 1, height: ((bits >>> 14) & 0x3fff) + 1 };
  }
  if (chunk === 'VP8 ' && b.length >= 30 && b[23] === 0x9d && b[24] === 0x01 && b[25] === 0x2a) {
    return { format: 'webp', width: u16le(b, 26) & 0x3fff, height: u16le(b, 28) & 0x3fff };
  }
  return null;
}

function bmpInfo(b) {
  if (b.length < 26 || b[0] !== 0x42 || b[1] !== 0x4d) return null;
  const height = u32le(b, 22) | 0;
  return { format: 'bmp', width: u32le(b, 18), height: Math.abs(height) };
}

function icoInfo(b) {
  if (b.length < 22 || u16le(b, 0) !== 0 || u16le(b, 2) !== 1 || u16le(b, 4) < 1) return null;
  // The first entry; 0 means 256. A multi-size icon reports its first image, the count says the rest.
  return { format: 'ico', width: b[6] || 256, height: b[7] || 256, images: u16le(b, 4) };
}

/** Decodes bytes as UTF-8 text without a DOM (the engine-free path used by the gate and the page alike). */
function utf8(bytes) {
  return new TextDecoder().decode(bytes);
}

/** The svg element's own size from width/height (px or unitless) or the viewBox; null if neither is there. */
export function svgSize(text) {
  const open = /<svg\b[^>]*>/i.exec(text);
  if (!open) return null;
  const tag = open[0];
  const attr = (name) => new RegExp('\\s' + name + '\\s*=\\s*["\']([^"\']*)["\']', 'i').exec(tag)?.[1];
  const px = (value) => {
    const m = /^\s*([0-9]*\.?[0-9]+)\s*(px)?\s*$/.exec(value ?? '');
    return m ? Math.round(parseFloat(m[1])) : null;
  };
  let width = px(attr('width'));
  let height = px(attr('height'));
  const box = attr('viewBox')?.trim().split(/[\s,]+/).map(Number);
  if ((width === null || height === null) && box && box.length === 4 && box.every(Number.isFinite)) {
    width ??= Math.round(box[2]);
    height ??= Math.round(box[3]);
  }
  return width === null || height === null ? null : { width, height };
}

/** The ids of the <symbol> elements of an SVG sprite: the icon list of an emitted icon set. */
export function svgSymbols(text) {
  const ids = [];
  const re = /<symbol\b[^>]*?\sid\s*=\s*["']([^"']+)["']/gi;
  for (let m = re.exec(text); m !== null; m = re.exec(text)) ids.push(m[1]);
  return ids;
}

function nameTable(b, base) {
  const tables = u16be(b, base + 4);
  let nameOffset = -1;
  for (let i = 0; i < tables; i += 1) {
    const rec = base + 12 + i * 16;
    if (rec + 16 > b.length) return null;
    if (ascii(b, rec, 4) === 'name') { nameOffset = u32be(b, rec + 8); break; }
  }
  if (nameOffset < 0 || nameOffset + 6 > b.length) return null;
  const count = u16be(b, nameOffset + 2);
  const strings = nameOffset + u16be(b, nameOffset + 4);
  const out = new Map();
  for (let i = 0; i < count; i += 1) {
    const rec = nameOffset + 6 + i * 12;
    if (rec + 12 > b.length) break;
    const platform = u16be(b, rec);
    const id = u16be(b, rec + 6);
    const length = u16be(b, rec + 8);
    const start = strings + u16be(b, rec + 10);
    if (start + length > b.length) continue;
    let value;
    if (platform === 3 || platform === 0) {
      let s = '';
      for (let o = start; o + 1 < start + length; o += 2) s += String.fromCharCode(u16be(b, o));
      value = s;
    } else if (platform === 1) {
      value = ascii(b, start, length);
    } else continue;
    // Windows (3) wins over the Mac (1) string for the same id.
    if (!out.has(id) || platform === 3) out.set(id, value);
  }
  return out;
}

/** Family, style and license strings of a TrueType/OpenType font (ttf, otf, ttc first face); woff is named only. */
function fontInfo(b, ext) {
  if (b.length < 12) return null;
  const tag = ascii(b, 0, 4);
  if (tag === 'wOF2' || tag === 'wOFF') {
    // The name table is compressed inside the container; naming it is enough for the panel.
    return { format: tag === 'wOF2' ? 'woff2' : 'woff', note: 'name table is compressed; family and license are not read' };
  }
  let base = 0;
  let format = ext === 'otf' ? 'otf' : 'ttf';
  if (tag === 'ttcf') {
    if (b.length < 16) return null;
    base = u32be(b, 12);
    format = 'ttc';
  } else if (!(u32be(b, 0) === 0x00010000 || tag === 'OTTO' || tag === 'true')) {
    return null;
  } else if (tag === 'OTTO') format = 'otf';
  if (base + 12 > b.length) return null;
  const names = nameTable(b, base);
  const info = { format };
  if (names) {
    const pick = (id) => names.get(id);
    info.family = pick(16) ?? pick(1);
    info.style = pick(17) ?? pick(2);
    info.fullName = pick(4);
    info.copyright = pick(0);
    info.license = pick(13);
    info.licenseUrl = pick(14);
    for (const key of Object.keys(info)) if (info[key] === undefined) delete info[key];
  }
  return info;
}

/**
 * Reads what the bytes say about an asset.
 * @param {string} path project-relative path (the extension picks the reader)
 * @param {Uint8Array} bytes
 * @returns {{ class: string, bytes: number, format?: string, width?: number, height?: number, family?: string,
 *   style?: string, fullName?: string, copyright?: string, license?: string, licenseUrl?: string,
 *   symbols?: string[], images?: number, note?: string, warnings: string[] }}
 */
export function inspectAsset(path, bytes) {
  const cls = assetClass(path);
  const out = { class: cls, bytes: bytes.length, warnings: [] };
  let found = null;
  if (cls === 'image') {
    const ext = extOf(path);
    if (ext === 'svg') {
      const text = utf8(bytes);
      found = { format: 'svg', ...(svgSize(text) ?? {}) };
      const symbols = svgSymbols(text);
      if (symbols.length > 0) out.symbols = symbols;
    } else {
      found = pngInfo(bytes) ?? jpegInfo(bytes) ?? gifInfo(bytes) ?? webpInfo(bytes) ?? bmpInfo(bytes) ?? icoInfo(bytes);
      if (found && found.format !== (ext === 'jpg' ? 'jpeg' : ext)) {
        out.warnings.push('content is ' + found.format + ' but the extension says .' + ext);
      }
    }
    if (found && (found.width === undefined || found.width === 0 || found.height === 0)) delete found.width, delete found.height;
    if (!found) out.warnings.push('not a readable ' + ext + ' image');
  } else if (cls === 'font') {
    found = fontInfo(bytes, extOf(path));
    if (!found) out.warnings.push('not a readable font file');
    else if (!found.license && !found.licenseUrl && found.format !== 'woff' && found.format !== 'woff2') {
      out.warnings.push('the font carries no license string; record its license next to the file');
    }
  }
  if (found) Object.assign(out, found);
  return out;
}

/**
 * A budget hint for the assets of a project against the preview size budget.
 * @param {number} assetBytes total bytes of the project's assets
 * @param {number} budgetBytes the preview budget the page is held to
 * @returns {{ level: 'ok' | 'warn' | 'over', ratio: number, message: string }}
 */
export function budgetHint(assetBytes, budgetBytes) {
  const ratio = budgetBytes > 0 ? assetBytes / budgetBytes : Infinity;
  const kib = (n) => (n / 1024).toFixed(1) + ' KiB';
  if (ratio > 1) return { level: 'over', ratio, message: 'assets are ' + kib(assetBytes) + ', over the ' + kib(budgetBytes) + ' budget' };
  if (ratio >= 0.8) return { level: 'warn', ratio, message: 'assets use ' + Math.round(ratio * 100) + '% of the ' + kib(budgetBytes) + ' budget' };
  return { level: 'ok', ratio, message: 'assets use ' + Math.round(ratio * 100) + '% of the ' + kib(budgetBytes) + ' budget' };
}

// --- references ------------------------------------------------------------------------------------------

/** Resolves a relative specifier against the directory of `fromFile`; null when it escapes the project root. */
export function resolveSpecifier(fromFile, spec) {
  const parts = spec.startsWith('./') || spec.startsWith('../') ? fromFile.split('/').slice(0, -1) : [];
  for (const seg of spec.split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') { if (parts.length === 0) return null; parts.pop(); } else parts.push(seg);
  }
  return parts.join('/');
}

/** The specifier that reaches `target` from `fromFile`, in the same style (relative or project-rooted) as `style`. */
export function respecify(fromFile, target, style) {
  if (!style.startsWith('./') && !style.startsWith('../')) return target;
  const from = fromFile.split('/').slice(0, -1);
  const to = target.split('/');
  let i = 0;
  while (i < from.length && i < to.length - 1 && from[i] === to[i]) i += 1;
  const up = from.length - i;
  return (up === 0 ? './' : '../'.repeat(up)) + to.slice(i).join('/');
}

const SOURCE_EXT = new Set(['ts', 'tsx', 'js', 'jsx', 'mjs', 'json', 'css']);

/**
 * Every reference to `assetPath` in text files: quoted strings in source and url(...) in CSS.
 * A reference is a specifier that resolves (relative to its file, or project-rooted) to the asset path.
 * @param {readonly { path: string, text: string }[]} files
 * @returns {{ file: string, offset: number, length: number, specifier: string, kind: 'string' | 'url' }[]}
 */
export function findAssetReferences(files, assetPath) {
  const refs = [];
  for (const file of files) {
    if (!SOURCE_EXT.has(extOf(file.path))) continue;
    const text = file.text;
    const test = (specifier, offset, kind) => {
      if (/^[a-z][a-z0-9+.-]*:/i.test(specifier) || specifier.startsWith('//') || specifier.startsWith('#')) return;
      const clean = specifier.split(/[?#]/)[0];
      if (resolveSpecifier(file.path, clean) === assetPath) {
        refs.push({ file: file.path, offset, length: clean.length, specifier: clean, kind });
      }
    };
    const quoted = /(["'`])((?:(?!\1)[^\\\n])+)\1/g;
    for (let m = quoted.exec(text); m !== null; m = quoted.exec(text)) test(m[2], m.index + 1, 'string');
    if (extOf(file.path) === 'css') {
      const url = /url\(\s*([^)"'\s][^)\s]*)\s*\)/gi;
      for (let m = url.exec(text); m !== null; m = url.exec(text)) test(m[1], m.index + m[0].indexOf(m[1]), 'url');
    }
  }
  return refs.sort((a, b) => (a.file < b.file ? -1 : a.file > b.file ? 1 : a.offset - b.offset));
}

/**
 * Plans renaming an asset and rewriting its references, as per-file edits that apply back to front.
 * @param {readonly { path: string, text: string }[]} files text files of the project
 * @param {readonly string[]} allPaths every project path (to refuse a collision)
 * @returns {{ ok: true, from: string, to: string, edits: { file: string, offset: number, length: number, newText: string }[],
 *   references: number, warnings: string[] } | { ok: false, reason: string }}
 */
export function planAssetRename(files, allPaths, from, to) {
  if (!allPaths.includes(from)) return { ok: false, reason: from + ' is not in this project' };
  if (from === to) return { ok: false, reason: 'the new path is the old path' };
  if (to === '' || to.startsWith('/') || to.split('/').includes('..') || to.includes('\\') || to.endsWith('/')) {
    return { ok: false, reason: 'the new path must be project-relative, with no .. and no trailing slash' };
  }
  if (allPaths.includes(to) || allPaths.some((p) => p.startsWith(to + '/'))) return { ok: false, reason: to + ' is already taken' };
  const warnings = [];
  if (extOf(from) !== extOf(to)) warnings.push('the extension changes from .' + extOf(from) + ' to .' + extOf(to));
  const refs = findAssetReferences(files, from);
  const edits = refs.map((r) => ({ file: r.file, offset: r.offset, length: r.length, newText: respecify(r.file, to, r.specifier) }));
  return { ok: true, from, to, edits, references: edits.length, warnings };
}

/**
 * Checks a replacement upload against the asset it replaces: same class, readable, size change, and a
 * dimension or family change worth a notice. The path stays, so no reference moves.
 * @returns {{ ok: true, before: object, after: object, warnings: string[] } | { ok: false, reason: string }}
 */
export function planAssetReplace(path, oldBytes, newBytes, maxBytes) {
  const cls = assetClass(path);
  if (cls === 'other') return { ok: false, reason: path + ' is not an image or a font' };
  if (newBytes.length === 0) return { ok: false, reason: 'the replacement is empty' };
  if (newBytes.length > maxBytes) return { ok: false, reason: 'the replacement is ' + newBytes.length + ' bytes, over the ' + maxBytes + ' byte limit' };
  const before = inspectAsset(path, oldBytes);
  const after = inspectAsset(path, newBytes);
  if (after.warnings.some((w) => w.startsWith('not a readable'))) return { ok: false, reason: after.warnings[0] };
  const warnings = after.warnings.slice();
  if (before.width !== undefined && after.width !== undefined && (before.width !== after.width || before.height !== after.height)) {
    warnings.push('size changes from ' + before.width + 'x' + before.height + ' to ' + after.width + 'x' + after.height);
  }
  if (before.family !== undefined && after.family !== undefined && before.family !== after.family) {
    warnings.push('font family changes from ' + before.family + ' to ' + after.family);
  }
  return { ok: true, before, after, warnings };
}
