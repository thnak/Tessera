// SPDX-License-Identifier: MIT

// Type declarations for `preview-state.js`, the studio's environment vocabulary and its validation
// (issue #169, P5).
//
// Like `share.js` beside it, this file stays `.js` on purpose: it is a pure ES module with no DOM and no
// React, and `scripts/ci/preview_share_roundtrip.mjs` imports it directly under Node so the codec test keeps
// testing the shipped source (ADR-054, Consequences). See `share.d.ts` for the longer note.

/** The file name the studio compiles under; a share link omits it while it stays the default. */
export const DEFAULT_FILE: string;

/** The viewport presets the toolbar offers: `[width, height, label]` in logical pixels. */
export const VIEWPORTS: ReadonlyArray<readonly [number, number, string]>;

/** The DPR values the toolbar offers. */
export const DPRS: readonly number[];

/** The theme ids the toolbar offers; `system` follows `prefers-color-scheme`. */
export const THEMES: readonly string[];

/** The state a page starts from; each field is also the decoder's fallback for a missing one. */
export const DEFAULT_STATE: Readonly<ShareState>;

/** The per-file source limit (`jsx-subset-v1.md` section 15) and the envelope cap derived from it. */
export const MAX_SOURCE_BYTES: number;
export const MAX_ENVELOPE_BYTES: number;

/** The state a share link carries; the keys are the fragment's one-character envelope fields. */
export interface ShareState {
  /** The source text, verbatim: the single source of truth, never a sample id and never a diff. */
  source: string;
  /** The source path, which every `uiId` is derived from. */
  file: string;
  /** Viewport width in logical pixels. */
  width: number;
  /** Viewport height in logical pixels. */
  height: number;
  /** Device pixel ratio. */
  dpr: number;
  /** `system` | `light` | `dark`. */
  theme: string;
}

/** A refusal the page branches on by `code`, rather than by matching a message. */
export class ShareError extends Error {
  readonly code: "too-large" | "malformed" | "version" | "invalid" | "unsupported";
  constructor(code: string, message: string);
}

/**
 * Validates and normalizes a state object. Every field is optional and falls back to `DEFAULT_STATE`,
 * except that an explicitly *invalid* value is an error: a page that recorded 0 DPR or a 3000-pixel "phone"
 * has a bug, and silently substituting a default would hide it in the one place -- a shared link -- where
 * the mistake is hardest to trace back.
 */
export function normalizeState(
  state?: Partial<ShareState>,
  options?: { viewportAllowed?: (width: number, height: number) => boolean },
): ShareState;

/** `"390x844"` -> `{ width: 390, height: 844 }`; throws on anything the toolbar could not produce. */
export function parseViewport(text: string): { width: number; height: number };
