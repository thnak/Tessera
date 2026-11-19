// SPDX-License-Identifier: MIT

// The local-preference half of the P5 chrome (issue #169), kept DOM-free so both the React toolbar and the
// Node smoke test read the same code.
//
// `preview-state.js` is the vocabulary; this module is what the page *does* with it: remember the toolbar
// locally and read it back at startup. There is no share link (the studio has no cloud storage to share to),
// so the URL is never read or written for state.
//
// Reading `localStorage` can throw (a sandboxed frame, a private-mode browser, a quota), and none of those are
// page errors -- preferences are an optimization, not state.

export {
  CUSTOM_SIZE_RANGE,
  DEFAULT_FILE,
  FOLD_POSTURES,
  FOLD_POSTURE_LABELS,
  PROFILE_IDS,
  SKIN_IDS,
  THEMES,
  VIEWPORTS,
  DPRS,
  parseViewport,
} from "./preview-state";

import { parseViewport } from "./preview-state";

/** The locally stored toggles, kept apart from the fragment so a local toggle cannot rewrite the URL. */
const STORAGE_KEY = "tessera.preview.p5";

/** The fields a previous visit may restore. The source never is one of them. */
const PREFERENCE_KEYS = [
  "viewport",
  "dpr",
  "theme",
  "file",
  "foldPosture",
  "skin",
  "profile",
] as const;

/** The toolbar's values, as the strings a `<select>` holds. */
export interface Controls {
  viewport: string;
  dpr: string;
  theme: string;
  /**
   * The simulated fold posture (issue #408): `""` (no fold), `flat`, `book` or `tabletop`. It is a
   * layout input exactly like the viewport, so a change re-lays the preview out rather than waiting
   * for a Run.
   */
  foldPosture: string;
  /**
   * The simulated device skin (issue #171's skins slice): a directory name under `skins/`, or `""`
   * for no frame. A layout input like the viewport: the skin's derived viewer size and its insets
   * are what the engine lays out, so a change re-lays the preview out in one pass.
   */
  skin: string;
  /** The device profile (issue #447): a bundled id, `custom`, or `""` for none. */
  profile: string;
}

/** The stored preference fields: the toolbar's controls plus the file name. */
type Preferences = Partial<Controls & { file: string }>;

/**
 * The page's environment state: the toolbar's toggles plus the entry file name.
 *
 * `from` says where it arrived from: a stored `local` preference or the `default`.
 */
export interface EnvironmentState {
  source: string;
  file: string;
  width: number;
  height: number;
  dpr: number;
  theme: string;
  /** The simulated fold posture; `""` is no fold (issue #408). */
  foldPosture: string;
  /** The simulated device skin; `""` is no frame (issue #171's skins slice). */
  skin: string;
  /** The device profile; `""` is none (issue #447). */
  profile: string;
  from?: "local" | "default";
}

/** The toolbar a page starts from before the fragment or storage says otherwise. */
export const DEFAULT_CONTROLS: Controls = Object.freeze({
  viewport: "320x240",
  dpr: "1",
  theme: "system",
  foldPosture: "",
  skin: "",
  profile: "",
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
 * The state the page should start from: the locally stored toggles, else `fallback`. The source never comes
 * from storage; it is the open project's.
 */
export function initialState(fallback: EnvironmentState): EnvironmentState {
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
      foldPosture:
        typeof stored.foldPosture === "string" ? stored.foldPosture : fallback.foldPosture,
      skin: typeof stored.skin === "string" ? stored.skin : fallback.skin,
      profile: typeof stored.profile === "string" ? stored.profile : fallback.profile,
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
