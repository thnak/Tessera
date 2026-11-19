// SPDX-License-Identifier: MIT

// Region 1 of the studio workspace (issue #431): the **studio chrome** -- the header and the two shared
// actions. The view switch is *not* here any more: three top-level tabs were the demo this issue
// replaces, and the switch now lives in the preview column it controls (`regions/PreviewColumn.tsx`).
//
// The header keeps what belongs to the page: the title, the per-view subtitle, the sample gallery toggle
// and the share link. Everything that names the *preview* -- viewport, DPR, theme, fold posture -- moved
// into the preview column's own toolbar (`EnvironmentToolbar` below) because that is what it is: preview
// chrome, read by two of the three preview panes and carried into the share link by the third.
//
// The ids are load-bearing. `scripts/ci/preview_page_checks.mjs` renders exactly these components with
// `react-dom/server` and asserts the ids and roles the page's own code looks up, so renaming one here
// without renaming its ref somewhere is a check failure rather than a runtime null (ADR-054 decision 6).

import type { RefObject } from "react";
import { CUSTOM_SIZE_RANGE, DPRS, FOLD_POSTURES, FOLD_POSTURE_LABELS, PROFILE_IDS, SKIN_IDS, THEMES, VIEWPORTS } from "../lib/preferences";
import type { DeviceProfile } from "../lib/device-profile";
import type { ViewDefinition } from "../lib/views";

/** The toolbar's toggles, as the strings a `<select>` holds. */
export interface ControlValues {
  viewport: string;
  dpr: string;
  theme: string;
  /**
   * The simulated fold posture (issue #408): `""` for no fold, else `flat` | `book` | `tabletop`.
   * A layout input like the viewport, so changing it re-lays the preview out in one pass.
   */
  foldPosture: string;
  /**
   * The simulated device skin (issue #171's skins slice): a directory name under `skins/`, or `""`
   * for "no frame". A layout input like the viewport: the skin's derived viewer size and its insets
   * are what the engine lays out, so changing it re-lays the preview out in one pass.
   */
  skin: string;
  /**
   * The device profile (issue #447): a bundled profile id, `custom` (a size typed into the custom entry), or
   * `""` for the custom size the page always started with. `""` and `custom` are one option in the picker.
   */
  profile: string;
}

/**
 * The page header: the title, the per-view subtitle and the two shared actions.
 *
 * The subtitle describes the view the preview column is showing; the page owns it so a switch inside the
 * preview column cannot leave the previous view's sentence behind.
 */
export function PageHeader({
  view,
  onToggleGallery,
  galleryOpen,
  onShare,
  shareStatus,
  shareStatusRef,
}: {
  view: ViewDefinition;
  onToggleGallery: () => void;
  galleryOpen: boolean;
  onShare: () => void;
  shareStatus: string;
  shareStatusRef: RefObject<HTMLSpanElement | null>;
}) {
  return (
    <header className="page-header">
      <div className="page-title">
        <h1>Tessera Studio</h1>
        <p className="page-subtitle" id="page-hint">
          {view.hint}
        </p>
      </div>
      <div className="page-meta">
        <div className="toolbar-actions">
          <button
            id="toggle-gallery"
            type="button"
            aria-expanded={galleryOpen}
            aria-controls="gallery"
            onClick={onToggleGallery}
          >
            Samples
          </button>
          <button id="share" type="button" onClick={onShare}>
            Copy share link
          </button>
          <span
            className="share-status"
            id="share-status"
            role="status"
            aria-live="polite"
            ref={shareStatusRef}
          >
            {shareStatus}
          </span>
        </div>
      </div>
    </header>
  );
}

/** One labelled `<select>`; the toolbar has five and they differ only in their options. */
function Control({
  label,
  name,
  value,
  values,
  format,
  onChange,
  disabled,
}: {
  label: string;
  name: keyof ControlValues;
  value: string;
  values: readonly string[];
  format: (value: string) => string;
  onChange: (name: keyof ControlValues, value: string) => void;
  /** Options that cannot be chosen; a rejected skin is one (section 5.2: "it is not offered"). */
  disabled?: (value: string) => boolean;
}) {
  return (
    <div className="control">
      <span className="control-label">{label}</span>
      <select
        id={`control-${name}`}
        aria-label={label}
        value={value}
        onChange={(event) => onChange(name, event.currentTarget.value)}
      >
        {values.map((option) => (
          <option key={option} value={option} disabled={disabled?.(option) ?? false}>
            {format(option)}
          </option>
        ))}
      </select>
    </div>
  );
}

/**
 * The preview column's own toolbar: the environment the preview is laid out and painted in.
 *
 * Every view reads it; Scene and Canvas apply it, Diagnostics carries it into the share link and says
 * so. Keeping one toolbar for three panes is the merge's point (#392) -- before it, the canvas page had
 * none at all -- and keeping it *inside* the preview column is this issue's: the control belongs to the
 * thing it changes.
 *
 * `skins` is the validated bundled set (issue #171's skins slice). It is a prop rather than a fetch
 * here because the page owns the load: a manifest that failed is reported once, in the page's own
 * banner, instead of once per render of this component.
 *
 * The picker's options are the documented vocabulary (`SKIN_IDS`), not the loaded set, so the control
 * renders identically before and after the manifests arrive - a `<select>` whose options appear
 * asynchronously would show an empty control on first paint, and the SSR chrome the Node gates render
 * would disagree with the page. A skin that failed validation is `disabled` instead of absent, which
 * is the same "not offered" rule with a visible reason.
 */
export function EnvironmentToolbar({
  controls,
  onChange,
  skins,
  profiles,
  profile,
  keepState,
  onKeepState,
  onCustomSize,
}: {
  controls: ControlValues;
  onChange: (name: keyof ControlValues, value: string) => void;
  skins: readonly { id: string; name: string }[];
  /** The validated bundled device profiles (issue #447); the picker offers these and the custom size. */
  profiles: readonly { id: string; name: string }[];
  /** The profile in force, or `null` for the custom size the page started with. */
  profile: DeviceProfile | null;
  /** The "keep state on device change" setting (section 3); off by default, so a switch restarts. */
  keepState: boolean;
  onKeepState: (keep: boolean) => void;
  /** A size typed into the custom entry: it makes the ad hoc profile and resizes in place. */
  onCustomSize: (width: number, height: number) => void;
}) {
  // The preset list plus the size now in force, so a typed or profile-applied size is a visible option and
  // the `<select>` never shows a blank for a value it does not list.
  const presetValues = VIEWPORTS.map(([width, height]) => `${width}x${height}`);
  const viewportValues = presetValues.includes(controls.viewport) ? presetValues : [...presetValues, controls.viewport];
  const dprValues = DPRS.map((value) => String(value));
  if (!dprValues.includes(controls.dpr)) {
    dprValues.push(controls.dpr);
  }
  const customSize = controls.profile === "" || controls.profile === "custom";
  function commitCustomSize(width: string, height: string): void {
    const w = Math.round(Number(width));
    const h = Math.round(Number(height));
    const [low, high] = CUSTOM_SIZE_RANGE;
    if (Number.isFinite(w) && Number.isFinite(h) && w >= low && w <= high && h >= low && h <= high) {
      onCustomSize(w, h);
    }
  }
  const [currentWidth, currentHeight] = controls.viewport.split("x");
  const loadedProfiles = new Set(profiles.map((entry) => entry.id));
  const loaded = new Set(skins.map((skin) => skin.id));
  return (
    <div className="toolbar" role="group" aria-label="Preview environment">
      <div className="controls" id="controls">
        {/*
          The skin picker (issue #171's skins slice). `designer-studio-v1.md` section 5.2 assigns the
          bundled skins to #171, and this is the control the fold-posture block below used to say did
          not exist yet.

          `""` is "no frame", which is the pre-#171 page: a visitor who ignores the control sees
          exactly what they saw before, and a page whose manifests all failed to load still previews.
        */}
        {/*
          The device (issue #447, `studio-device-profiles-v1.md` section 2): a bundled profile, or the
          custom size. `""` is the custom size, which is the pre-profile page, so a visitor who ignores the
          control sees what they saw before. Picking a profile applies its size, DPR, skin and fold and
          restarts the app (section 3); the setting below keeps state when the `os` does not change.
        */}
        <Control
          label="Device"
          name="profile"
          value={customSize ? "" : controls.profile}
          values={PROFILE_IDS.filter((id) => id !== "custom")}
          format={(value) => {
            if (value === "") {
              return "Custom size";
            }
            const match = profiles.find((entry) => entry.id === value);
            return match ? match.name : value;
          }}
          disabled={(value) => value !== "" && !loadedProfiles.has(value)}
          onChange={onChange}
        />
        <Control
          label="Skin"
          name="skin"
          value={controls.skin}
          values={[...SKIN_IDS]}
          format={(value) => {
            if (value === "") {
              return "None";
            }
            const match = skins.find((skin) => skin.id === value);
            return match ? match.name : value;
          }}
          disabled={(value) => value !== "" && !loaded.has(value)}
          onChange={onChange}
        />
        {customSize ? (
          <>
            <Control
              label="Viewport"
              name="viewport"
              value={controls.viewport}
              values={viewportValues}
              format={(value) => {
                const match = VIEWPORTS.find(([width, height]) => `${width}x${height}` === value);
                return match ? match[2] : value;
              }}
              onChange={onChange}
            />
            <div className="control custom-size">
              <span className="control-label">Size</span>
              <input
                id="control-width"
                type="number"
                aria-label="Custom width"
                min={CUSTOM_SIZE_RANGE[0]}
                max={CUSTOM_SIZE_RANGE[1]}
                defaultValue={currentWidth}
                key={`w${currentWidth}`}
                onKeyDown={(event) => {
                  if (event.key === "Enter") {
                    event.currentTarget.blur();
                  }
                }}
                onBlur={(event) => commitCustomSize(event.currentTarget.value, currentHeight)}
              />
              <span aria-hidden="true">{"\u00d7"}</span>
              <input
                id="control-height"
                type="number"
                aria-label="Custom height"
                min={CUSTOM_SIZE_RANGE[0]}
                max={CUSTOM_SIZE_RANGE[1]}
                defaultValue={currentHeight}
                key={`h${currentHeight}`}
                onKeyDown={(event) => {
                  if (event.key === "Enter") {
                    event.currentTarget.blur();
                  }
                }}
                onBlur={(event) => commitCustomSize(currentWidth, event.currentTarget.value)}
              />
            </div>
          </>
        ) : profile !== null ? (
          <div className="control profile-size" id="profile-size">
            <span className="control-label">Size</span>
            <span className="profile-size-value">
              {controls.viewport.replace("x", "\u00d7")} @ {controls.dpr}
              {"\u00d7"}
            </span>
          </div>
        ) : null}
        <Control
          label="DPR"
          name="dpr"
          value={controls.dpr}
          values={dprValues}
          format={(value) => value + "\u00d7"}
          onChange={onChange}
        />
        <Control
          label="Theme"
          name="theme"
          value={controls.theme}
          values={[...THEMES]}
          format={(value) => value}
          onChange={onChange}
        />
        {/*
          The fold posture (issue #408, `designer-studio-v1.md` section 6: "Fold posture
          (foldables)" -> `viewport.set` `foldPosture`).

          It stays always present rather than appearing only for a `foldable` skin. The original
          reason ("a control that waits for a skin category nobody can select would be dead chrome")
          no longer holds now that the picker exists, but the posture is a layout input of its own: a
          developer previewing a book posture on the generic desktop skin is a legitimate thing to
          do, and hiding the control would make the posture depend on a choice it does not depend on.
          `None` is the unfolded desktop case, so a visitor who ignores the control sees exactly what
          they saw before.
        */}
        <Control
          label="Fold"
          name="foldPosture"
          value={controls.foldPosture}
          values={[...FOLD_POSTURES]}
          format={(value) => FOLD_POSTURE_LABELS[value] ?? value}
          onChange={onChange}
        />
        {/* Section 3's setting: a device change applies size, DPR, insets and fold to the running app in
            place. It is honoured only between profiles of the same `os`; a change of `os` always restarts. */}
        <label className="control keep-state" title="Apply a device change in place instead of restarting. Only between profiles of the same OS; a different OS always restarts.">
          <input
            id="control-keep-state"
            type="checkbox"
            checked={keepState}
            onChange={(event) => onKeepState(event.currentTarget.checked)}
          />
          <span className="control-label">Keep state on device change</span>
        </label>
      </div>
    </div>
  );
}
