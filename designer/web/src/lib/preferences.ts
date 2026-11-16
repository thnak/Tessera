// SPDX-License-Identifier: MIT

// The local-preference and share-link half of the P5 chrome (issue #169), kept DOM-free so both the React
// toolbar and the Node smoke test read the same code.
//
// `share.js` is the codec and `preview-state.js` is the vocabulary; this module is what the page *does* with
// them: decode the URL's fragment, remember the toolbar locally, and produce the link. The two are
// deliberately separate concerns: a local toggle must never rewrite the URL, and a link must override
// whatever a visitor last chose here.
//
// Reading `localStorage` can throw (a sandboxed frame, a private-mode browser, a quota), and none of those are
// page errors -- preferences are an optimization, not state.

export {
  DEFAULT_FILE,
  THEMES,
  VIEWPORTS,
  DPRS,
  ShareError,
  parseViewport,
} from "./preview-state";
export type { ShareState } from "./preview-state";

import { DEFAULT_FILE, ShareError, parseViewport } from "./preview-state";
import type { ShareState } from "./preview-state";
import { encodeShare, stateFromHash } from "./share";

/** The locally stored toggles, kept apart from the fragment so a local toggle cannot rewrite the URL. */
const STORAGE_KEY = "tessera.preview.p5";

/** The fields a previous visit may restore. The source never is one of them. */
const PREFERENCE_KEYS = ["viewport", "dpr", "theme", "file"] as const;

/** The toolbar's values, as the strings a `<select>` holds. */
export interface Controls {
  viewport: string;
  dpr: string;
  theme: string;
}

/** The stored preference fields: the toolbar's three plus the file name a link carried. */
type Preferences = Partial<Controls & { file: string }>;

/**
 * The page's environment state: the toolbar's three toggles plus the file a share link names.
 *
 * `from` says where it arrived from, which is what the page uses to decide whether to mention the link at
 * all: a `link` that failed to decode is a banner, a `local` preference is not.
 */
export interface EnvironmentState {
  source: string;
  file: string;
  width: number;
  height: number;
  dpr: number;
  theme: string;
  from?: "link" | "local" | "default";
}

/** The toolbar a page starts from before the fragment or storage says otherwise. */
export const DEFAULT_CONTROLS: Controls = Object.freeze({
  viewport: "320x240",
  dpr: "1",
  theme: "system",
});

function safeStorage(): Storage | null {
  try {
    // Merely reading `localStorage` throws in some sandboxed frames, so probe it here.
    return globalThis.localStorage ?? null;
  } catch {
    return null;
  }
}

/** The locally stored toggles, or `null` when there are none (or storage is unavailable). */
export function readLocalPreferences(storage: Storage | null = safeStorage()): Preferences | null {
  if (!storage) {
    return null;
  }
  try {
    const text = storage.getItem(STORAGE_KEY);
    return text ? (JSON.parse(text) as Preferences) : null;
  } catch {
    // A private-mode browser or a corrupted value: preferences are an optimization, not state.
    return null;
  }
}

/** Writes the toggles locally. Failure is silent on purpose: the fragment still carries them. */
export function writeLocalPreferences(
  preferences: Preferences,
  storage: Storage | null = safeStorage(),
): void {
  if (!storage) {
    return;
  }
  const kept: Preferences = {};
  for (const key of PREFERENCE_KEYS) {
    const value = preferences[key];
    if (value !== undefined) {
      (kept as Record<string, unknown>)[key] = value;
    }
  }
  try {
    storage.setItem(STORAGE_KEY, JSON.stringify(kept));
  } catch {
    /* quota or a blocked store: the URL is the durable copy */
  }
}

/** Writes the toolbar's position to `localStorage` **without** a fragment. */
export function rememberLocally(controls: Controls): void {
  writeLocalPreferences(controls);
}

/**
 * The state the page should start from: the URL's fragment when it carries one, else the locally stored
 * toggles, else `fallback`.
 *
 * A fragment wins over local storage because a link is an explicit instruction from whoever shared it; local
 * storage is only the visitor's last toolbar position. A fragment that fails to decode throws `ShareError`,
 * and the caller shows it: a broken link must say so rather than load the default source and look like the
 * link never arrived.
 */
export async function initialState(
  fallback: EnvironmentState,
  hash: string | null | undefined = globalThis.location?.hash,
): Promise<EnvironmentState> {
  const shared = await stateFromHash(hash);
  if (shared) {
    return { ...shared, from: "link" };
  }
  const stored = readLocalPreferences();
  if (stored) {
    // Only the environment fields are ever read back: the source comes from the sample (or the visitor's
    // editor), never from a previous session, so opening the page is not a surprise.
    const viewport = parseStoredViewport(stored.viewport);
    const dpr = Number(stored.dpr);
    return {
      ...fallback,
      file: typeof stored.file === "string" && stored.file !== "" ? stored.file : fallback.file,
      width: viewport ? viewport.width : fallback.width,
      height: viewport ? viewport.height : fallback.height,
      dpr: Number.isFinite(dpr) && dpr > 0 ? dpr : fallback.dpr,
      theme: typeof stored.theme === "string" && stored.theme !== "" ? stored.theme : fallback.theme,
      from: "local",
    };
  }
  return { ...fallback, from: "default" };
}

/** `"390x844"` when the stored string is one the toolbar could have produced, else `null`. */
function parseStoredViewport(text: unknown): { width: number; height: number } | null {
  if (typeof text !== "string") {
    return null;
  }
  try {
    return parseViewport(text);
  } catch {
    return null;
  }
}

/** The state object for `encodeShare` from the toolbar's string-valued controls. */
export function shareStateOf(
  source: string,
  controls: Controls,
  file: string = DEFAULT_FILE,
): ShareState {
  const viewport = parseViewport(controls.viewport);
  return {
    source,
    file,
    width: viewport.width,
    height: viewport.height,
    dpr: Number(controls.dpr),
    theme: String(controls.theme),
  };
}

/**
 * Writes the share link to the address bar (`history.replaceState`, so it does not add a back step) and
 * returns the text a "Copy link" button should put on the clipboard.
 */
export async function shareToLocation(
  state: ShareState,
  history: Pick<History, "replaceState"> | undefined = globalThis.history,
  location: Pick<Location, "origin" | "pathname" | "search"> | undefined = globalThis.location,
): Promise<string> {
  const payload = await encodeShare(state);
  // Deliberately no `view` parameter: the fragment is the state, and the view is the page's own. A link
  // made on the Scene tab still opens on whichever view the address bar's `?view=` names.
  const prefix = location ? `${location.origin}${location.pathname}${location.search}` : "";
  const url = `${prefix}#p5=${payload}`;
  try {
    history?.replaceState(null, "", url);
  } catch {
    // A `data:` or `file:` origin can refuse the rewrite; the returned text is still shareable.
  }
  return url;
}

/** The banner's sentence for a link that did not decode; the code is shown when it is a `ShareError`. */
export function linkProblemMessage(error: unknown): string {
  return error instanceof ShareError
    ? `This share link could not be opened (${error.code}): ${error.message}`
    : `This share link could not be opened: ${String(error)}`;
}
