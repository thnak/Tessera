// SPDX-License-Identifier: MIT

// The preview's one font, fetched by the page and handed to the module as **data** (issue #387).
//
// **Why the page fetches it and the module does not.** The Canvas module has no filesystem (it is
// built without `-sNODERAWFS`, docs/design/compiler-wasm.md section 2.1) and the page's CSP allows no
// remote script or font host (ADR-045 decisions 1-3). So the bytes come from this site's own static
// assets -- `designer/web/public/fonts/`, copied into the built site by Vite -- and are passed through
// `tessera_canvas_set_font(ptr, size)`. Nothing leaves the browser, and the module embeds no font, so
// the Wasm download stays out of the P6 size budget.
//
// **Why it is a module of its own.** It is the only part of the Canvas view that talks to the network
// at all, and its failure mode is not the WebGPU failure mode: a browser can have a perfectly good
// device and no font, and the preview must then say "text is not rendered" rather than "the canvas is
// broken". Keeping the fetch, its cache and its sentence apart from the draw path is what makes that
// distinction cheap to hold.
//
// **The face.** IBM Plex Sans Regular, subset to Latin, OFL-1.1 (see `THIRD_PARTY_NOTICES.md` and
// `docs/specs/dependencies.md`). It is fetched once per page and cached in memory for the session; a
// second Canvas Run reuses it rather than paying for the download again.

/** Where the font lives in the built site. Same origin, static, no CDN. */
export const FONT_URL = "fonts/IBMPlexSans-Latin.ttf";

/** The face's own name, for the pane's status sentence. Must match what the bytes report. */
export const FONT_FAMILY = "IBM Plex Sans";

/** What a font load produced: the bytes, or a sentence naming what failed. */
export type FontResult =
  | { bytes: Uint8Array; error?: undefined }
  | { bytes?: undefined; error: string };

// One page-wide cache. A `Map` rather than a promise: the fetch is small and the second caller of a
// failed load must see the failure, not a resolved empty buffer.
let cached: Uint8Array | null = null;

/**
 * Fetches the preview font from this site, or returns a sentence naming what failed.
 *
 * `fetch` is same-origin and relative to the document's base, so the page works under a project path
 * (`/Tessera/`) exactly as the module URLs do (`lib/module-url.ts` has the same reasoning for the
 * Wasm files). A response that is not `ok` is an error and not an empty buffer: an empty buffer would
 * reach the module as "no font", which is the state the pane shows differently from "the fetch
 * failed".
 */
export async function loadPreviewFont(): Promise<FontResult> {
  if (cached !== null) {
    return { bytes: cached };
  }
  const base =
    typeof document !== "undefined" && document.baseURI
      ? document.baseURI
      : globalThis.location?.href ?? "http://localhost/";
  const url = new URL(FONT_URL, base).href;
  let response: Response;
  try {
    response = await fetch(url);
  } catch (error) {
    return { error: `the font could not be fetched from ${url}: ${String(error)}` };
  }
  if (!response.ok) {
    return { error: `the font fetch failed: ${response.status} ${response.statusText} (${url})` };
  }
  let bytes: Uint8Array;
  try {
    bytes = new Uint8Array(await response.arrayBuffer());
  } catch (error) {
    return { error: `the font response could not be read: ${String(error)}` };
  }
  if (bytes.byteLength === 0) {
    return { error: `the font at ${url} is empty` };
  }
  cached = bytes;
  return { bytes };
}

/**
 * Copies `bytes` into `module`'s heap and calls `tessera_canvas_set_font`, then releases the copy.
 *
 * The three-way return is the module's: 1 accepted, 0 empty data, -1 refused. Each maps to a
 * sentence, because a bare number in the pane tells a designer nothing. The heap copy is released in
 * a `finally` on both paths -- the module copies the bytes into its own `FontFace`, so it never keeps
 * this pointer.
 */
export function handOverFont(
  module: { _malloc(size: number): number; _free(pointer: number): void; [name: string]: unknown },
  bytes: Uint8Array,
): { ok: true; error?: undefined } | { ok?: undefined; error: string } {
  const setFont = module._tessera_canvas_set_font;
  if (typeof setFont !== "function") {
    return { error: "The module this page loaded cannot accept a font (`tessera_canvas_set_font`)." };
  }
  const pointer = module._malloc(bytes.byteLength);
  if (!pointer) {
    return { error: "The preview module is out of memory for the font bytes." };
  }
  try {
    // `HEAPU8` is an Emscripten view over the module's memory; a fresh view is taken per call
    // because memory growth replaces the underlying buffer.
    const heap = (module as { HEAPU8?: Uint8Array }).HEAPU8;
    if (!heap) {
      return { error: "The preview module exposes no HEAPU8, so its memory cannot be written." };
    }
    heap.set(bytes, pointer);
    const accepted = (setFont as (pointer: number, size: number) => number)(pointer, bytes.byteLength);
    if (accepted === 1) {
      return { ok: true };
    }
    if (accepted === 0) {
      return { error: "The preview module received no font bytes." };
    }
    return { error: "The preview module refused the font bytes as unusable." };
  } finally {
    module._free(pointer);
  }
}

/**
 * Reads the module's font status report (`tessera_canvas_font_status`), or null when the module does
 * not export it.
 *
 * `{ loaded, family, glyphsDropped, atlasPages, atlasEvictions, error }`: the pane shows `family`
 * when text is on, and `error` when it is not. `glyphsDropped` is the bounded atlas saying which
 * glyphs it could not fit this session (see `tools/codegen/preview_text.hpp`).
 */
export interface FontStatus {
  fontVersion?: number;
  loaded?: boolean;
  family?: string;
  glyphsDropped?: number;
  atlasPages?: number;
  atlasEvictions?: number;
  error?: string;
}

export function readFontStatus(module: {
  _tessera_canvas_font_status?: unknown;
  _tessera_free?: (pointer: number) => void;
  UTF8ToString?: (pointer: number) => string;
}): FontStatus | null {
  const exported = module._tessera_canvas_font_status;
  const free = module._tessera_free;
  const utf8 = module.UTF8ToString;
  if (typeof exported !== "function" || typeof free !== "function" || typeof utf8 !== "function") {
    return null;
  }
  const pointer = (exported as () => number)();
  if (!pointer) {
    return null;
  }
  try {
    return JSON.parse(utf8(pointer)) as FontStatus;
  } catch {
    return null;
  } finally {
    free(pointer);
  }
}

/** What the font steps need from a module: the heap for the handover, and the status reader. */
export type FontModule = Parameters<typeof handOverFont>[0] & Parameters<typeof readFontStatus>[0];

/**
 * The whole font step for one device: fetch the bytes, hand them over, and report the face.
 *
 * Returns null when text is drawn, or the sentence saying why it is not. A failed font is the
 * documented degraded state (boxes, no glyphs), never a failed canvas, so the caller shows the
 * sentence and keeps drawing. It lives here beside the fetch and the handover so the Canvas session
 * states the policy once per device instead of restating the three steps.
 */
export async function loadFontFor(
  module: FontModule,
  report: (family: string | null) => void,
): Promise<string | null> {
  const loaded = await loadPreviewFont();
  if (loaded.error !== undefined) {
    return loaded.error;
  }
  const handover = handOverFont(module, loaded.bytes);
  if (handover.error !== undefined) {
    return handover.error;
  }
  const status = readFontStatus(module);
  // The module copies the bytes into its own `FontFace`, so the cached buffer stays on the page for a
  // later module instance rather than being kept alive per device.
  report(status?.loaded === true ? (status.family ?? FONT_FAMILY) : null);
  return status?.loaded === true ? null : (status?.error ?? "the module reported no font");
}
