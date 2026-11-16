// SPDX-License-Identifier: MIT

// Type declarations for `share.js`, the share-link codec (issue #169, P5).
//
// **Why the codec is still `.js`.** ADR-054's risk section names this file: `share.js` and
// `preview-state.js` must stay pure ES modules (no DOM, no React) so
// `scripts/ci/preview_share_roundtrip.mjs` keeps importing the *same bytes* the page ships, under Node,
// with no build step. Rewriting either as TypeScript would mean the 19-codec-check gate tests a build
// artifact instead of the shipped source. The declarations below give the React side a checked contract
// without moving the codec.
//
// `share.js` re-exports `DEFAULT_FILE` and `ShareError` from `preview-state.js`; both are declared once in
// `preview-state.d.ts` and re-declared here as the re-exports they are.

import type { ShareState } from "./preview-state";

export type { ShareState };

/** The only envelope version this module reads or writes. */
export const SHARE_FORMAT_VERSION: number;

/** Encoded payload cap in characters (8 KiB; the module's header explains why). */
export const MAX_FRAGMENT_CHARS: number;

/** The source file name a link omits while it stays the default. */
export const DEFAULT_FILE: string;

/** A refusal the page branches on by `code`. */
export { ShareError } from "./preview-state";

/**
 * Encodes a state into the fragment payload (no leading `#`).
 *
 * `{ compress: false }` forces the uncompressed mode, which is how the fallback path is exercised on a
 * platform that does have `CompressionStream`. Throws `ShareError` rather than returning a partial result.
 */
export function encodeShare(state: ShareState, options?: { compress?: boolean }): Promise<string>;

/** Decodes a bare payload, a `#p5=...` fragment, or a whole URL back into a state. */
export function decodeShare(text: string | null | undefined): Promise<ShareState>;

/** The state in `hash`, or `null` when the hash carries no `#p5=` fragment. */
export function stateFromHash(hash: string | null | undefined): Promise<ShareState | null>;
