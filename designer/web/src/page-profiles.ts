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
  const size = skin !== null ? skin.viewer : next.size;
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
