// SPDX-License-Identifier: MIT

// Which scene export one Run calls, and the argument shape that export takes (issues #390, #408 and P5).
//
// **Why this is its own module.** It is a statement about the *artifact*, not about the session: which
// exports a module carries decides which of the six scene documents the page can ask for, and the answer
// changes when the page and the module were built from different commits. The session's job is the Wasm
// lifecycle (import, instantiate, render, status); this file's job is the ABI shape. Splitting them keeps
// `scene-session.ts` under the soft source limit and keeps the fallback rules readable in one place.
//
// **The families, and why there are six exports.** The P2 parity gate pins the bytes of the plain export
// at the native CLI's default rectangle (320x240, DPR 1), so a default run must keep calling it. The sized
// export exists for a moved toolbar; the folded export (issue #408) for a simulated posture, which the
// sized one cannot express; and each of the three gained a `*_styled` sibling (issue #390) that takes the
// author stylesheet as one more string. A page published against an older module falls back to the
// unstyled export and says so rather than failing.

import type { WasmModule } from "../lib/wasm-call";

// The preview's default layout rectangle (wasm_scene_entry.cpp's kDefaultWidth/Height/DpiScale): the
// plain `tessera_scene_json` export lays the tree out at exactly this, so the sized export is only
// needed when the toolbar has moved off it.
export const DEFAULT_VIEWPORT = { width: 320, height: 240 } as const;
export const DEFAULT_DPR = 1;

/** The export one Run should call, and how to marshal its arguments. */
export interface ExportChoice {
  /** The module export's name. */
  name: string;
  /** The numbers `callExport` appends after the strings (w/h/dpr for the sized shapes). */
  numbers: number[];
  /**
   * Whether the fold posture is the first extra string.
   *
   * `callExport` marshals every string, then every number, so the strings' *order* is the ABI: the folded
   * family takes `(source, file, component, posture[, stylesheet])`, and the sized/plain families take
   * `(source, file, component[, stylesheet])`.
   */
  postureFirst: boolean;
  /** True when the chosen export carries the author stylesheet, so the caller must append it. */
  styled: boolean;
}

// The plain export is the parity gate's contract (byte-identical to the native CLI's 320x240 DPR 1) and
// is what a default run must keep calling; the sized one is for a moved toolbar. The fallback covers an
// artifact built before P5 landed next to this page.
function usesSizedExport(
  module: WasmModule,
  viewport: { width: number; height: number },
  dpr: number,
): boolean {
  return isMoved(viewport, dpr) && typeof module._tessera_scene_json_sized === "function";
}

/** Whether the toolbar has moved off the plain export's default rectangle. */
export function isMoved(viewport: { width: number; height: number }, dpr: number): boolean {
  return (
    viewport.width !== DEFAULT_VIEWPORT.width ||
    viewport.height !== DEFAULT_VIEWPORT.height ||
    dpr !== DEFAULT_DPR
  );
}

/**
 * Whether this artifact carries the styled export the chosen shape needs (issue #390).
 *
 * A partial set is possible -- a module built from a tree between two commits -- so the test is per
 * shape rather than one global "the styled family exists": a fold plus a stylesheet needs the folded
 * styled export, and a moved viewport needs the sized one.
 */
function hasStyledExports(module: WasmModule, foldable: boolean, moved: boolean): boolean {
  if (foldable) {
    return typeof module._tessera_scene_json_folded_styled === "function";
  }
  if (moved) {
    return typeof module._tessera_scene_json_styled_sized === "function";
  }
  return typeof module._tessera_scene_json_styled === "function";
}

/**
 * The export this run should call, and why.
 *
 * `_tessera_scene_json_folded` (issue #408) is the only export that carries the fold posture, so a
 * foldable preview has to use it. A run with no posture keeps calling the plain export when nothing
 * else moved, because that byte-for-byte equality with the native CLI's default is what the parity
 * gate asserts; a moved viewport keeps using the sized export, and a fold takes precedence because
 * the sized one cannot express it.
 *
 * The `*_styled` family (issue #390) is the same set with the author stylesheet as one more string.
 * It is preferred whenever the author wrote CSS, because a preview that ignored the sheet would show
 * the built-in look while claiming to show their screen. When the artifact predates those exports the
 * run falls back to the unstyled one and `styled` comes back false, so the status line can say the
 * sheet is not applied rather than implying it is.
 *
 * The `folded` export falls back to the sized/plain pair when the artifact predates it: a page
 * published against an older module then shows an unfolded preview and says so, rather than failing.
 */
export function chooseExport(
  module: WasmModule,
  viewport: { width: number; height: number },
  dpr: number,
  posture: string,
  wantsStyling: boolean,
): ExportChoice {
  const moved = isMoved(viewport, dpr);
  const foldable = posture !== "";
  const styled = wantsStyling && hasStyledExports(module, foldable, moved);
  if (foldable && typeof module._tessera_scene_json_folded === "function") {
    // The folded family takes the posture and (styled) the stylesheet, then w/h/dpr.
    return {
      name: styled ? "_tessera_scene_json_folded_styled" : "_tessera_scene_json_folded",
      numbers: [viewport.width, viewport.height, dpr],
      postureFirst: true,
      styled,
    };
  }
  if (styled && moved) {
    return {
      name: "_tessera_scene_json_styled_sized",
      numbers: [viewport.width, viewport.height, dpr],
      postureFirst: false,
      styled,
    };
  }
  if (styled) {
    return { name: "_tessera_scene_json_styled", numbers: [], postureFirst: false, styled };
  }
  if (usesSizedExport(module, viewport, dpr)) {
    return {
      name: "_tessera_scene_json_sized",
      numbers: [viewport.width, viewport.height, dpr],
      postureFirst: false,
      styled,
    };
  }
  return { name: "_tessera_scene_json", numbers: [], postureFirst: false, styled };
}
