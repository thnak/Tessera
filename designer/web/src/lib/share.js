// SPDX-License-Identifier: MIT

// The share-link *codec* of the preview pages (issue #169, P5 of
// docs/design/reference-equantic-ui.md section 7.4). The format is specified in
// docs/specs/designer-studio-v1.md section 7.1; the state vocabulary lives in `preview-state.js`.
//
// This file is a *pure* ES module: no DOM, no `window`, no `fetch`. Its only asynchronous work is the
// browser's own `CompressionStream`/`DecompressionStream`. That is what makes the round trip testable
// under Node (`scripts/ci/preview_share_roundtrip.mjs`) without a browser and without the Wasm module:
// the page and the test import the same bytes of code.
//
// ## The fragment
//
// `#p5=<payload>`, where `<payload>` is one mode character followed by base64url data:
//
//   z  deflate-raw compressed (the default: smallest, and what every current browser supports)
//   g  gzip compressed (fallback for a browser with gzip but not deflate-raw)
//   u  uncompressed base64url (fallback for a browser with no compression stream at all)
//
// The decoded bytes are a UTF-8 JSON envelope. Keys are one character because the fragment is the
// product here: every byte is a byte somebody pastes into a chat window or an issue.
//
//   {"v":1,"s":"<source>","w":320,"h":240,"d":1,"t":"system"[,"f":"ui/Counter.tsx"][,"p":"book"][,"k":"generic-phone-android"]}
//
//   v  format version, must equal SHARE_FORMAT_VERSION (an unknown one is an error, never a guess)
//   s  the source text, verbatim (the single source of truth; never a sample id and never a diff)
//   w  viewport width in logical pixels; h its height
//   d  device pixel ratio (DPR)
//   t  theme: "system" | "light" | "dark"
//   f  source file name, optional; absent means DEFAULT_FILE (so the common case pays no bytes)
//   p  simulated fold posture (issue #408): "flat" | "book" | "tabletop"; absent means no fold, so
//      an unfolded preview pays no bytes and a pre-#408 link decodes to the same state it always did
//   k  simulated skin (issue #171's skins slice): a directory name under `skins/`; absent means no
//      frame, so a pre-#171 link decodes to the same state it always did
//
// ## The size cap, and what happens when a link exceeds it
//
// A fragment longer than `MAX_FRAGMENT_CHARS` is **refused**, on the way out and on the way in, with
// `ShareError("too-large")` and a sentence naming the actual size and the limit. It is never
// truncated: a truncated link is a link that silently loads different source than the one that was
// shared, which is the one failure a share feature must not have. 8 KiB is not a browser limit (URLs
// are far longer than that everywhere); it is the point past which a URL stops being pasteable, and a
// project too big for it is what the studio's zip export is for (designer-studio-v1.md section 7.3
// item 6). The decompressed side is capped too, enforced *while* reading, so a small hostile fragment
// cannot expand into a large allocation.

import {
  DEFAULT_FILE,
  MAX_ENVELOPE_BYTES,
  ShareError,
  normalizeState,
} from './preview-state.js';

export { DEFAULT_FILE, ShareError } from './preview-state.js';

/** The only envelope version this module reads or writes. */
export const SHARE_FORMAT_VERSION = 1;

/** Encoded payload cap, in characters (see the header for why 8 KiB). */
export const MAX_FRAGMENT_CHARS = 8192;

const encoder = new TextEncoder();

function hasStream(kind, format) {
  const Stream = kind === 'compress' ? globalThis.CompressionStream : globalThis.DecompressionStream;
  if (typeof Stream !== 'function') {
    return false;
  }
  try {
    // The constructor validates the format string, so this is the feature test that matters.
    new Stream(format);
    return true;
  } catch {
    return false;
  }
}

function decodeUtf8(bytes, what) {
  try {
    return new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  } catch {
    throw new ShareError('malformed', `the ${what} is not valid UTF-8`);
  }
}

// `btoa` takes latin1, and a single `apply` would overflow the argument list on a large source.
function toBase64Url(bytes) {
  let binary = '';
  const chunk = 0x8000;
  for (let offset = 0; offset < bytes.length; offset += chunk) {
    binary += String.fromCharCode.apply(null, bytes.subarray(offset, offset + chunk));
  }
  return globalThis
    .btoa(binary)
    .replace(/\+/g, '-')
    .replace(/\//g, '_')
    .replace(/=+$/, '');
}

function fromBase64Url(text) {
  if (!/^[A-Za-z0-9_-]*$/.test(text)) {
    throw new ShareError('malformed', 'the fragment is not base64url');
  }
  const padded = text.replace(/-/g, '+').replace(/_/g, '/');
  const binary = globalThis.atob(padded + '='.repeat((4 - (padded.length % 4)) % 4));
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) {
    bytes[index] = binary.charCodeAt(index);
  }
  return bytes;
}

async function compressBytes(bytes, format) {
  const stream = new Blob([bytes]).stream().pipeThrough(new globalThis.CompressionStream(format));
  return new Uint8Array(await new Response(stream).arrayBuffer());
}

// A cap enforced *while* reading, not after: a hostile 8 KiB fragment must not get to allocate a
// gigabyte before somebody checks the total. A stream that is not deflate at all rejects from
// `read()`, and that is a malformed link rather than a page crash, so the two are separated here.
async function decompressCapped(bytes, format, cap) {
  const stream = new Blob([bytes]).stream().pipeThrough(new globalThis.DecompressionStream(format));
  const reader = stream.getReader();
  const chunks = [];
  let total = 0;
  try {
    for (;;) {
      let chunk;
      try {
        chunk = await reader.read();
      } catch {
        throw new ShareError('malformed', `the fragment is not valid ${format} data`);
      }
      if (chunk.done) {
        break;
      }
      total += chunk.value.byteLength;
      if (total > cap) {
        throw new ShareError(
          'too-large',
          `the link's source expands past ${cap} bytes; a project that large needs the studio's ` +
            'zip export, not a link',
        );
      }
      chunks.push(chunk.value);
    }
  } finally {
    // Releasing a reader whose stream already errored is legal and keeps the error path leak-free.
    reader.releaseLock();
  }
  const out = new Uint8Array(total);
  let offset = 0;
  for (const value of chunks) {
    out.set(value, offset);
    offset += value.byteLength;
  }
  return out;
}

function envelopeOf(state) {
  // Fixed key order, so the same state always produces the same fragment (the test asserts it).
  return {
    v: SHARE_FORMAT_VERSION,
    s: state.source,
    w: state.width,
    h: state.height,
    d: state.dpr,
    t: state.theme,
    ...(state.file === DEFAULT_FILE ? {} : { f: state.file }),
    // `p` is the fold posture (issue #408). An unfolded preview omits the key, so every link a
    // pre-#408 page wrote decodes identically and every link this page writes for the common case
    // costs zero extra bytes. `f` keeps meaning the file name: renaming it would make every link in
    // the wild decode with the wrong field.
    ...(state.foldPosture ? { p: state.foldPosture } : {}),
    // `k` is the simulated skin (issue #171's skins slice). Omitted for "no frame", which is the
    // pre-#171 page, so an old link decodes to the same state it always did and a link made without
    // a skin pays no bytes. `s` is the source and `p` the posture, so `k` is the next free letter
    // that reads as "skin".
    ...(state.skin ? { k: state.skin } : {}),
  };
}

/**
 * Encodes a share state into the fragment payload (no leading `#`).
 *
 * `{ compress: false }` forces mode `u`, which is how the fallback path is tested on a platform that
 * does have `CompressionStream`. Throws `ShareError` rather than returning a partial result.
 */
export async function encodeShare(state, options = {}) {
  const normalized = normalizeState(state);
  const json = encoder.encode(JSON.stringify(envelopeOf(normalized)));

  let mode = 'u';
  let bytes = json;
  if (options.compress !== false) {
    if (hasStream('compress', 'deflate-raw')) {
      mode = 'z';
      bytes = await compressBytes(json, 'deflate-raw');
    } else if (hasStream('compress', 'gzip')) {
      mode = 'g';
      bytes = await compressBytes(json, 'gzip');
    }
  }

  const payload = mode + toBase64Url(bytes);
  if (payload.length > MAX_FRAGMENT_CHARS) {
    throw new ShareError(
      'too-large',
      `this state encodes to ${payload.length} characters, over the ${MAX_FRAGMENT_CHARS} ` +
        'character link limit; shorten the source or export the project instead of sharing a link',
    );
  }
  return payload;
}

/**
 * Decodes a fragment payload back into a state. Accepts a bare payload, a `#p5=...` fragment, or a
 * whole URL, because that is what a person pastes into the address bar.
 */
export async function decodeShare(text) {
  let payload = String(text ?? '').trim();
  const hash = payload.indexOf('#');
  if (hash >= 0) {
    payload = payload.slice(hash + 1);
  }
  if (payload.startsWith('p5=')) {
    payload = payload.slice(3);
  }
  if (payload === '') {
    throw new ShareError('malformed', 'the link has no fragment');
  }
  if (payload.length > MAX_FRAGMENT_CHARS) {
    throw new ShareError(
      'too-large',
      `the fragment is ${payload.length} characters, over the ${MAX_FRAGMENT_CHARS} character limit`,
    );
  }

  const mode = payload[0];
  const body = payload.slice(1);
  if (!'zgu'.includes(mode) || body === '') {
    throw new ShareError('malformed', 'the fragment does not start with a known mode');
  }
  const bytes = fromBase64Url(body);

  let json = bytes;
  if (mode !== 'u') {
    const format = mode === 'z' ? 'deflate-raw' : 'gzip';
    if (!hasStream('decompress', format)) {
      throw new ShareError(
        'unsupported',
        `this browser cannot decompress ${format}; the link needs a browser with ` +
          'DecompressionStream',
      );
    }
    json = await decompressCapped(bytes, format, MAX_ENVELOPE_BYTES);
  } else if (bytes.length > MAX_ENVELOPE_BYTES) {
    throw new ShareError('too-large', 'the fragment expands past the source limit');
  }

  let envelope;
  try {
    envelope = JSON.parse(decodeUtf8(json, 'fragment'));
  } catch (error) {
    if (error instanceof ShareError) {
      throw error;
    }
    throw new ShareError('malformed', 'the fragment is not a JSON envelope');
  }
  if (envelope === null || typeof envelope !== 'object' || Array.isArray(envelope)) {
    throw new ShareError('malformed', 'the fragment is not a JSON object');
  }
  if (envelope.v !== SHARE_FORMAT_VERSION) {
    throw new ShareError(
      'version',
      `the link is share format ${JSON.stringify(envelope.v)}; this page reads ` +
        `${SHARE_FORMAT_VERSION}. A newer link cannot be read by an older page`,
    );
  }
  return normalizeState({
    source: envelope.s,
    file: envelope.f === undefined ? DEFAULT_FILE : envelope.f,
    width: envelope.w,
    height: envelope.h,
    dpr: envelope.d,
    theme: envelope.t,
    // Absent means "no fold", which is what a pre-#408 link says and what the page's default is.
    foldPosture: envelope.p === undefined ? '' : envelope.p,
    // Absent means "no frame", which is what a pre-#171 link says and what the page's default is.
    skin: envelope.k === undefined ? '' : envelope.k,
  });
}

/** A state read from a fragment, or null when `hash` carries no P5 fragment. */
export async function stateFromHash(hash) {
  if (typeof hash !== 'string' || !/(^#|&)p5=/.test(hash)) {
    return null;
  }
  return decodeShare(hash);
}
