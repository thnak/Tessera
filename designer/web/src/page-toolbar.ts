// SPDX-License-Identifier: MIT

// The toolbar's handlers, split out of `App.tsx` (the 500-line limit): picking a device, typing a custom size,
// changing one control, and telling every mounted view about the move. The device chip and the Device menu are
// projections of these through the command registry (`page-commands.ts`).

import { useCallback, useEffect, useRef, type MutableRefObject } from "react";
import { customProfile, mayKeepState, type DeviceProfile } from "./lib/device-profile";
import { DPRS, VIEWPORTS, rememberLocally } from "./lib/preferences";
import { deviceSwitch, skinSwitch } from "./page-profiles";
import type { PageStateSetters } from "./page-state";
import type { ControlValues } from "./regions/StudioChrome";
import type { Skin } from "./views/skins";

export function useToolbarControls({
  state_,
  controls,
  controlsRef,
  profilesRef,
  skinsRef,
  keepStateRef,
  sessions,
  restart,
}: {
  state_: PageStateSetters;
  controls: ControlValues;
  controlsRef: MutableRefObject<ControlValues>;
  profilesRef: MutableRefObject<{ selected: DeviceProfile | null; profiles: DeviceProfile[] }>;
  skinsRef: MutableRefObject<Skin[]>;
  keepStateRef: MutableRefObject<boolean>;
  sessions: Record<string, { onEnvironmentChange?: () => void } | undefined>;
  restart: () => void;
}) {
  // Set by a device switch that must restart the app; consumed by the effect below once every session has
  // heard the new environment, which is section 3's order (`viewport.set`, then the restart).
  const pendingRestart = useRef(false);
  // ---- the toolbar ---------------------------------------------------------------------------------
  // Picking a device (issue #447, section 3): `viewport.set` (size, DPR, skin, fold), then a restart unless the
  // person asked to keep state and the `os` is unchanged. The custom size is the pre-profile page.
  const switchDevice = useCallback((id: string) => {
    const current = controlsRef.current;
    const previous = profilesRef.current.selected;
    let result: { controls: ControlValues; restart: boolean };
    if (id === "") {
      const [width, height] = current.viewport.split("x").map(Number);
      const ad = customProfile({ width, height }, Number(current.dpr), previous);
      const plain =
        VIEWPORTS.some(([w, h]) => w === width && h === height) && DPRS.includes(Number(current.dpr));
      result = {
        controls: { ...current, profile: plain ? "" : "custom", skin: "", foldPosture: "" },
        restart: !(keepStateRef.current && mayKeepState(previous, ad)),
      };
    } else {
      const next = profilesRef.current.profiles.find((candidate) => candidate.id === id);
      if (next === undefined) {
        return;
      }
      result = deviceSwitch(previous, next, current, skinsRef.current, keepStateRef.current);
    }
    pendingRestart.current = result.restart;
    rememberLocally(result.controls);
    state_.setControls(result.controls);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Picking a skin (Device > Skin) is a layout input, not a device switch: the skin's viewer rectangle
  // becomes the viewport and the running evaluation is re-laid out in place, with no restart. The rule
  // itself is `skinSwitch` in `page-profiles.ts`, next to `deviceSwitch`, because the two must agree on
  // what a skin means for the size. "None" and an id that has not loaded keep the current size.
  const switchSkin = useCallback((id: string) => {
    const result = skinSwitch(controlsRef.current, id, skinsRef.current);
    rememberLocally(result.controls);
    state_.setControls(result.controls);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // A size typed into the custom entry makes the ad hoc profile and resizes in place: no restart, the same
  // as moving the preset list always did.
  const onCustomSize = useCallback((width: number, height: number) => {
    state_.setControls((previous) => {
      const preset = VIEWPORTS.some(([w, h]) => w === width && h === height);
      const next = {
        ...previous,
        viewport: `${width}x${height}`,
        profile: preset && DPRS.includes(Number(previous.dpr)) ? "" : "custom",
      };
      rememberLocally(next);
      return next;
    });
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const onControlChange = useCallback(
    (name: keyof ControlValues, value: string) => {
      if (name === "profile") {
        switchDevice(value);
        return;
      }
      if (name === "skin") {
        switchSkin(value);
        return;
      }
      state_.setControls((previous) => {
        const next = { ...previous, [name]: value };
        rememberLocally(next);
        return next;
      });
    },
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [switchDevice, switchSkin],
  );

  // Every mounted view hears about a toolbar move, not only the visible one: a Scene pane that is hidden
  // when the toolbar changes would otherwise show bounds from a viewport the toolbar no longer names the
  // moment it is opened. The first run is skipped -- it is the fragment decode, not a visitor's change.
  const firstControls = useRef(true);
  useEffect(() => {
    if (firstControls.current) {
      firstControls.current = false;
      return;
    }
    for (const session of Object.values(sessions)) {
      session?.onEnvironmentChange?.();
    }
    // A device switch that must restart does so now that the new size is in force (section 3's order).
    if (pendingRestart.current) {
      pendingRestart.current = false;
      restart();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [controls]);

  return { switchDevice, switchSkin, onCustomSize, onControlChange };
}
