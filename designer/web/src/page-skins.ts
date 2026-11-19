// SPDX-License-Identifier: MIT

// The page's skins state (issue #171's skins slice; `designer-studio-v1.md` sections 5.1 and 5.2).
//
// **What this hook owns.** Three things the page needs and no session does:
//
//   1. the bundled manifests, loaded once and validated (`views/skin-load.ts`);
//   2. the selected skin, resolved from the toolbar's `skin` value against the *validated* set, so a
//      manifest that failed is never offered and a link naming one falls back to no frame;
//   3. the theme attribute on `<html>`, which is what makes the toolbar's Theme control real and
//      therefore what makes "a theme switch retints the frame and the canvas in the same frame"
//      demonstrable (section 5.2's retint rule).
//
// **Why the theme attribute lives here.** It is the one DOM write the skins slice needs outside
// React's render cycle, and it is a property of the *document*, not of a component: the frame's
// colours are CSS custom properties on `:root`, so setting `data-theme` retints the frame and every
// other surface in one cascade. A component-local style would retint the frame and leave the page
// behind it on the old tokens, which is the "frame and canvas disagree" failure in slow motion.
//
// **Why the load is a hook and not a module-level promise.** A module-level fetch would run on
// import, including in the Node gates that import this file's siblings; a hook runs in the browser
// only, and it can report a rejection into the page's banner. The load is idempotent and never
// re-runs, so a re-render costs nothing.

import { useEffect, useMemo, useState } from "react";
import { loadBundledSkins, type SkinLoadRejection } from "./views/skin-load";
import { cutoutRectOf, safeAreasOf } from "./views/skin-geometry";
import type { Skin } from "./views/skins";
import type { SkinSafeArea } from "./views/canvas-session";

/** What the page needs from the skins slice, as one object. */
export interface SkinsState {
  /** The validated bundled skins, in the spec's order. Empty until the load settles. */
  skins: Skin[];
  /** The manifests that were rejected, so the page can say which skin is missing and why. */
  rejected: SkinLoadRejection[];
  /** The selected skin, or `null` for "no frame" (the pre-#171 page). */
  selected: Skin | null;
  /**
   * The selected skin's safe area, in the shape the canvas session takes, or `null` for no skin.
   *
   * This is the ONE derivation the engine sees: `safeAreasOf` and `cutoutRectOf` are the geometry
   * contract of section 5.1, and nothing else in the page computes an inset.
   */
  safeArea: SkinSafeArea | null;
}

/**
 * Loads the bundled skins once and resolves the toolbar's selection against them.
 *
 * `skinId` is the toolbar's value (`""` for no frame). A value that names a rejected or unknown skin
 * resolves to `null` rather than to a guess: a link that names a skin this build does not have shows
 * the bare canvas, which is the pre-#171 page, instead of a frame whose manifest nobody validated.
 */
export function useSkins(skinId: string): SkinsState {
  const [skins, setSkins] = useState<Skin[]>([]);
  const [rejected, setRejected] = useState<SkinLoadRejection[]>([]);

  useEffect(() => {
    let live = true;
    void loadBundledSkins().then((loaded) => {
      // A page torn down mid-load must not set state on an unmounted tree. The load is idempotent,
      // so the next mount simply asks again.
      if (!live) {
        return;
      }
      setSkins(loaded.skins);
      setRejected(loaded.rejected);
    });
    return () => {
      live = false;
    };
  }, []);

  const selected = useMemo(
    () => (skinId === "" ? null : (skins.find((skin) => skin.id === skinId) ?? null)),
    [skins, skinId],
  );

  const safeArea = useMemo((): SkinSafeArea | null => {
    if (selected === null) {
      return null;
    }
    return { insets: safeAreasOf(selected), cutout: cutoutRectOf(selected) };
  }, [selected]);

  return { skins, rejected, selected, safeArea };
}

/**
 * Writes the toolbar's theme to `<html data-theme>` (issue #171's skins slice).
 *
 * `system` removes the attribute, so the `prefers-color-scheme` media query is the only source and
 * the default path is byte-identical to the pre-#171 page. `light`/`dark` pin the tokens, which is
 * what the Theme control has always claimed to do and, before this slice, did not.
 */
export function useThemeAttribute(theme: string): void {
  useEffect(() => {
    const root = document.documentElement;
    if (theme === "light" || theme === "dark") {
      root.dataset.theme = theme;
    } else {
      delete root.dataset.theme;
    }
  }, [theme]);
}
