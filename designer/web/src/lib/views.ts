// SPDX-License-Identifier: MIT

// The contract between React and the three preview views (ADR-054 decision 3).
//
// React renders the *chrome*: the tab strip, the toolbar, the pane headers, the status lines, the
// empty states and the boxes. What it does not render is anything the engine produces -- a diagnostic
// list, a scene snapshot, a state-cell table, a WebGPU canvas -- because those are documents the Wasm
// modules write, and putting them in a render cycle would mean re-rendering on every compile (and, for
// the canvas, handing a GPU surface to React to destroy and recreate).
//
// So the seam is an explicit one: a view **session** is created once, given the elements React committed
// (by `ref`), and thereafter owns only those subtrees. React never re-renders a node a session writes
// into, because every such node is rendered childless once and never gets React children again.
//
// This is the same split `designer-studio-v1.md` section 3.1 draws for the studio as a whole: the chrome
// is HTML/CSS, the preview is the engine's.

/** The pane's own status line, which React renders and a session updates. */
export interface StatusReporter {
  /** `text` is a sentence; `kind` selects the colour (see `.status.ok/.error/.warning`). */
  (text: string, kind?: "ok" | "error" | "warning" | null): void;
}

/** Which toolbar values a view has to read; the toolbar itself is React's. */
export interface EnvironmentReader {
  /** The toolbar's current viewport and DPR, as layout inputs. */
  (): { viewport: string; dpr: string };
  /** The source path the current sample is previewed under (the share link's file name). */
  file(): string;
}

/**
 * The one interface all three views implement, so the page's shared Run and Load-sample buttons can
 * drive whichever view is showing without knowing which one it is.
 *
 * Every method but `load` is optional at the *page* level: the page calls one only when it exists,
 * which is what lets Diagnostics carry no viewport and Canvas no sample of its own without three
 * special cases in the page.
 */
export interface ViewSession {
  /** Loads the view's Wasm module; the page calls this the first time the view is shown. */
  load(): Promise<void>;
  /** The shared Run button's action on this view. */
  run(): Promise<void> | void;
  /** "Load sample" on this view: its own sample (and script), or nothing when it has none. */
  loadSample?(): void;
  /** The pane became (in)visible: a hidden Canvas pane must not draw. */
  setVisible?(visible: boolean): void;
  /** The toolbar moved; on Scene and Canvas the toggles are real inputs. */
  onEnvironmentChange?(): void;
  /** A gallery pick landed in the shared editor. */
  onSample?(path: string): void;
}

/** One of the page's three views. */
export interface ViewDefinition {
  id: "diagnostics" | "scene" | "canvas";
  label: string;
  /** The page subtitle's sentence for this view. */
  hint: string;
  /** The hint under the shared editor: what Run will do to it on this view. */
  editorHint: string;
}

/** The three views, in tab order. */
export const VIEWS: readonly ViewDefinition[] = Object.freeze([
  {
    id: "diagnostics",
    label: "Diagnostics",
    hint: "Compiles your TSX with the same compiler the native build runs, in your browser.",
    editorHint:
      "Ctrl+Enter (or Cmd+Enter) runs. A diagnostics run has no viewport: the toolbar is carried " +
      "into the share link, not applied to the compiler.",
  },
  {
    id: "scene",
    label: "Scene",
    hint:
      "Evaluates your TSX with the same evaluator, style and layout engine, then lists the scene " +
      "snapshot and the state cells a click script produces.",
    editorHint:
      "Ctrl+Enter (or Cmd+Enter) runs. The viewport and DPR are layout inputs here: the toolbar " +
      "re-runs the layout at the new size.",
  },
  {
    id: "canvas",
    label: "Canvas",
    hint:
      "Paints the evaluated scene through WebGPU, on demand: the canvas redraws only when " +
      "something actually changed.",
    editorHint:
      "Ctrl+Enter (or Cmd+Enter) runs. The canvas is sized from the toolbar and the device pixel " +
      "ratio, and redraws only when something changed.",
  },
]);

/** The view a page without `?view=` shows. Diagnostics is the cheapest one and needs no GPU. */
export const DEFAULT_VIEW: ViewDefinition["id"] = "diagnostics";

/** The definition of `id`, or the default view's when the id is not one of the three. */
export function viewOf(id: string | null | undefined): ViewDefinition {
  return VIEWS.find((entry) => entry.id === id) ?? VIEWS[0];
}

/** The view id in a search string, or `null` when it names none of this page's views. */
export function viewFromSearch(search: string | null | undefined): ViewDefinition["id"] | null {
  const text = String(search ?? "");
  const query = text.startsWith("?") ? text.slice(1) : text;
  if (query === "") {
    return null;
  }
  let view: string | null = null;
  for (const part of query.split("&")) {
    const equals = part.indexOf("=");
    const key = equals < 0 ? part : part.slice(0, equals);
    if (key !== "view") {
      continue;
    }
    view = equals < 0 ? "" : decodeURIComponent(part.slice(equals + 1).replace(/\+/g, " "));
  }
  return VIEWS.some((entry) => entry.id === view) ? (view as ViewDefinition["id"]) : null;
}

/**
 * The view the page should show: the URL's `?view=` when it names one, else `fallback`.
 *
 * The fragment carries no view field (`designer-studio-v1.md` section 7.1 is frozen: `v`, `s`, `w`,
 * `h`, `d`, `t`, `f`), so a pre-merge `scene.html#p5=...` link can only say "I was a scene link"
 * through the *path* it arrived on. The redirect stubs translate the old path into `?view=`, which is
 * why this function never has to guess.
 */
export function resolveView(
  search: string | null | undefined,
  fallback: string = DEFAULT_VIEW,
): ViewDefinition["id"] {
  const named = viewFromSearch(search);
  if (named) {
    return named;
  }
  return VIEWS.some((entry) => entry.id === fallback)
    ? (fallback as ViewDefinition["id"])
    : DEFAULT_VIEW;
}

/**
 * Puts `?view=<id>` in the address bar, preserving the query's other parameters and the fragment.
 *
 * `replaceState` rather than a plain assignment: a tab click must not push a history entry, and a
 * reload after one must still land on the same view. A `file:`/`data:` origin can refuse the rewrite;
 * the tabs still work, so the failure is swallowed rather than shown.
 */
export function writeViewToUrl(
  viewId: string,
  history: Pick<History, "replaceState"> | undefined = globalThis.history,
  location: Pick<Location, "pathname" | "search" | "hash"> | undefined = globalThis.location,
): void {
  if (!history || typeof history.replaceState !== "function" || !location) {
    return;
  }
  const params = new URLSearchParams(location.search || "");
  if (viewId === DEFAULT_VIEW) {
    // The default view costs no bytes in the address bar, so the plain site URL stays plain.
    params.delete("view");
  } else {
    params.set("view", viewId);
  }
  const query = params.toString();
  const url = `${location.pathname}${query ? "?" + query : ""}${location.hash || ""}`;
  try {
    history.replaceState(null, "", url);
  } catch {
    /* an origin that refuses the rewrite: the tabs still switch the view */
  }
}
