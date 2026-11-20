// SPDX-License-Identifier: MIT

// The page's device-profile state (issue #447; `studio-device-profiles-v1.md` sections 2 and 3).
//
// **What this hook owns.** The bundled manifests, loaded once and validated (`lib/device-profile-load.ts`),
// and the selected profile resolved from the toolbar's `profile` value against the *validated* set, so a
// manifest that failed is never offered and a link naming one falls back to the custom size.
//
// **What it does not own.** Switching device is a page action (`deviceSwitch` below computes it, `App.tsx`
// applies it) because it changes the toolbar's controls *and* decides whether the app restarts; the hook
// only answers "which profile is this" and "what would switching to that one change".
//
// The load is a hook and not a module-level promise for the reason `page-skins.ts` gives: a module-level
// fetch would run on import, including in the Node gates that import this file's siblings.

import { useEffect, useMemo, useRef, useState } from "react";
import {
  CUSTOM_PROFILE_ID,
  customProfile,
  mayKeepState,
  type DeviceProfile,
} from "./lib/device-profile";
import { loadBundledProfiles, type ProfileLoadRejection } from "./lib/device-profile-load";
import type { ControlValues } from "./regions/StudioChrome";
import { viewerOf } from "./views/skin-geometry";
import type { Skin } from "./views/skins";

/** What the page needs from the profiles slice, as one object. */
export interface ProfilesState {
  /** The validated bundled profiles, in the picker's order. Empty until the load settles. */
  profiles: DeviceProfile[];
  /** The manifests that were rejected, so the page can say which profile is missing and why. */
  rejected: ProfileLoadRejection[];
  /** The profile in force: a bundled one, the ad hoc custom one, or `null` for the pre-profile page. */
  selected: DeviceProfile | null;
}

/** Whether the toolbar's `profile` value means "the custom size" (no profile, or the ad hoc one). */
export function isCustomSize(profileId: string): boolean {
  return profileId === "" || profileId === CUSTOM_PROFILE_ID;
}

/** Loads the bundled profiles once and resolves the toolbar's selection against them. */
export function useProfiles(controls: ControlValues): ProfilesState {
  const [profiles, setProfiles] = useState<DeviceProfile[]>([]);
  const [rejected, setRejected] = useState<ProfileLoadRejection[]>([]);
  // The custom profile inherits the operating system of the last bundled one, so a keep-state switch
  // into a custom size still has an `os` to compare (section 3).
  const lastBundled = useRef<DeviceProfile | null>(null);

  useEffect(() => {
    let live = true;
    void loadBundledProfiles().then((loaded) => {
      if (!live) {
        return;
      }
      setProfiles(loaded.profiles);
      setRejected(loaded.rejected);
    });
    return () => {
      live = false;
    };
  }, []);

  const selected = useMemo((): DeviceProfile | null => {
    if (controls.profile === "") {
      return null;
    }
    if (controls.profile === CUSTOM_PROFILE_ID) {
      const [width, height] = controls.viewport.split("x").map(Number);
      return customProfile({ width, height }, Number(controls.dpr), lastBundled.current);
    }
    return profiles.find((profile) => profile.id === controls.profile) ?? null;
  }, [profiles, controls.profile, controls.viewport, controls.dpr]);

  if (selected !== null && selected.id !== CUSTOM_PROFILE_ID) {
    lastBundled.current = selected;
  }
  return { profiles, rejected, selected };
}

/** The controls a switch to `next` writes, and whether the app restarts. */
export interface DeviceSwitch {
  controls: ControlValues;
  /** True when the app must restart (cold start, state reset); false only for a permitted keep-state switch. */
  restart: boolean;
}

/**
 * What switching to `next` does (section 3): `viewport.set` (size, DPR, fold; the skin carries the insets),
 * then a restart unless the person asked to keep state **and** the `os` is unchanged.
 *
 * The size is the skin's viewer rectangle when the skin has loaded, because that is the rectangle the screen
 * shows (`designer-studio-v1.md` section 5.1); until it has, the profile's own size stands in, and the two
 * are equal for the bundled set (`scripts/ci/preview_profiles.mjs` asserts it against the skin manifests).
 */
export function deviceSwitch(
  previous: DeviceProfile | null,
  next: DeviceProfile,
  controls: ControlValues,
  skins: readonly Skin[],
  keepState: boolean,
): DeviceSwitch {
  const skin = next.skin === null ? null : (skins.find((candidate) => candidate.id === next.skin) ?? null);
  const size = skin !== null ? viewerOf(skin) : next.size;
  return {
    controls: {
      ...controls,
      profile: next.id,
      viewport: `${size.width}x${size.height}`,
      dpr: String(next.dpr),
      skin: next.skin ?? "",
      // A foldable opens flat; every other device has no hinge.
      foldPosture: next.category === "foldable" ? "flat" : "",
    },
    restart: !(keepState && mayKeepState(previous, next)),
  };
}

/** The controls a skin change writes, and whether the app restarts. */
export interface SkinSwitch {
  controls: ControlValues;
  /** Always false: a skin is a layout input, not a different platform (see `skinSwitch`). */
  restart: boolean;
}

/**
 * What picking a skin writes: the skin's viewer rectangle as the viewport, and nothing else.
 *
 * **Why the viewport moves.** Section 5.1: "the size in `viewport.set` is the size the preview renders
 * at", and `viewerOf` is the one derivation of it. Picking a skin on its own (Device > Skin, not a
 * profile) used to write only `skin`, so the frame grew to the skin's screen while the engine kept
 * laying out the old size: a small picture in a large black field. `deviceSwitch` above already did
 * this for the profile path; this is the same rule for the skin-only path.
 *
 * **What does not move.** "None" has no skin to take a size from, and an id that does not resolve to a
 * loaded skin (the manifests load asynchronously) has none either: both keep the current viewport
 * rather than inventing a fallback. The insets are not written here at all -- `useSkins().safeArea`
 * hands them to the viewer, and `safeAreasOf`/`cutoutRectOf` are the one derivation of those.
 *
 * **No restart.** `ControlValues.skin` is documented as a layout input "like the viewport": the engine
 * re-lays the running evaluation out in place (`_tessera_canvas_set_viewport`, then the safe area, in
 * one `onEnvironmentChange`), which is what `device.setViewport`, `device.setDpr`, `device.setFold`
 * and the custom-size entry already do. Only a profile switch restarts, because only it changes the
 * platform, the available controls and the initial environment (section 3).
 */
export function skinSwitch(controls: ControlValues, skinId: string, skins: readonly Skin[]): SkinSwitch {
  const skin = skinId === "" ? null : (skins.find((candidate) => candidate.id === skinId) ?? null);
  if (skin === null) {
    // "None", or a skin this build does not have: the picker's value still moves, the size does not.
    return { controls: { ...controls, skin: skinId }, restart: false };
  }
  const size = viewerOf(skin);
  return {
    controls: { ...controls, skin: skin.id, viewport: `${size.width}x${size.height}` },
    restart: false,
  };
}
