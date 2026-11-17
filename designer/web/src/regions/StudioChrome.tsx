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
import { DPRS, FOLD_POSTURES, FOLD_POSTURE_LABELS, THEMES, VIEWPORTS } from "../lib/preferences";
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

/** One labelled `<select>`; the toolbar has four and they differ only in their options. */
function Control({
  label,
  name,
  value,
  values,
  format,
  onChange,
}: {
  label: string;
  name: keyof ControlValues;
  value: string;
  values: readonly string[];
  format: (value: string) => string;
  onChange: (name: keyof ControlValues, value: string) => void;
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
          <option key={option} value={option}>
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
 */
export function EnvironmentToolbar({
  controls,
  onChange,
}: {
  controls: ControlValues;
  onChange: (name: keyof ControlValues, value: string) => void;
}) {
  const viewportValues = VIEWPORTS.map(([width, height]) => `${width}x${height}`);
  return (
    <div className="toolbar" role="group" aria-label="Preview environment">
      <div className="controls" id="controls">
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
        <Control
          label="DPR"
          name="dpr"
          value={controls.dpr}
          values={DPRS.map((value) => String(value))}
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

          It is always present rather than appearing only for a `foldable` skin, because this page has
          no skin picker yet: `designer-studio-v1.md` section 5.2 assigns skins to #171, and a control
          that waits for a skin category nobody can select would be dead chrome. `None` is the unfolded
          desktop case, so a visitor who ignores the control sees exactly what they saw before.
        */}
        <Control
          label="Fold"
          name="foldPosture"
          value={controls.foldPosture}
          values={[...FOLD_POSTURES]}
          format={(value) => FOLD_POSTURE_LABELS[value] ?? value}
          onChange={onChange}
        />
      </div>
    </div>
  );
}
