// SPDX-License-Identifier: MIT

// Region 1 of the four `designer-studio-v1.md` section 3 names: the **studio chrome**.
//
// The header (title, subtitle, view tabs, build id), the environment toolbar (viewport, DPR, theme) and
// the actions beside them (Samples, Copy share link, their status). All of it is React, and all of it is
// *only* chrome: nothing here is ever drawn by the engine, and nothing here renders the previewed app
// (ADR-054 decision 3, ADR-045 decision 3).
//
// The ids are load-bearing. `scripts/ci/preview_page_checks.mjs` renders exactly these components with
// `react-dom/server` and asserts the ids and roles the page's own code looks up, so renaming one here
// without renaming its ref somewhere is a check failure rather than a runtime null (ADR-054 decision 6).
//
// The tablist is the page's only navigation: one tab stop, arrow keys inside it, `aria-selected`
// mirroring the visible pane. It is a real `role="tablist"` rather than styled buttons because the panes
// are real tab panels, and because `?view=scene` links mean the selected tab is state a visitor can
// arrive with.

import type { RefObject } from "react";
import { DPRS, FOLD_POSTURES, FOLD_POSTURE_LABELS, THEMES, VIEWPORTS } from "../lib/preferences";
import { VIEWS, type ViewDefinition } from "../lib/views";

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

/** The view tab strip (region 1's navigation). */
export function ViewTabs({
  current,
  onSelect,
}: {
  current: ViewDefinition["id"];
  onSelect: (id: ViewDefinition["id"]) => void;
}) {
  return (
    <nav className="page-nav" role="tablist" aria-label="Preview views">
      {VIEWS.map((view) => {
        const selected = view.id === current;
        return (
          <button
            key={view.id}
            id={`tab-${view.id}`}
            type="button"
            role="tab"
            className="tab"
            aria-controls={`view-${view.id}`}
            aria-selected={selected}
            // Roving tabindex: a tab strip is one tab stop, and the arrow keys move inside it.
            tabIndex={selected ? 0 : -1}
            onClick={() => onSelect(view.id)}
            onKeyDown={(event) => {
              const order = VIEWS.map((entry) => entry.id);
              const index = order.indexOf(current);
              let next: ViewDefinition["id"] | null = null;
              if (event.key === "ArrowRight") {
                next = order[(index + 1) % order.length];
              } else if (event.key === "ArrowLeft") {
                next = order[(index - 1 + order.length) % order.length];
              } else if (event.key === "Home") {
                next = order[0];
              } else if (event.key === "End") {
                next = order[order.length - 1];
              }
              if (next) {
                event.preventDefault();
                onSelect(next);
                // Moving focus with the selection is what makes the strip one tab stop that is usable:
                // without it the focus ring would stay on a tab that is no longer selected.
                document.getElementById(`tab-${next}`)?.focus();
              }
            }}
          >
            {view.label}
          </button>
        );
      })}
    </nav>
  );
}

/** The page header: title, the per-view subtitle, the tabs and the build id. */
export function PageHeader({
  view,
  buildId,
  buildIdTitle,
  onSelect,
  buildIdRef,
}: {
  view: ViewDefinition;
  buildId: string;
  buildIdTitle?: string;
  onSelect: (id: ViewDefinition["id"]) => void;
  buildIdRef: RefObject<HTMLDivElement | null>;
}) {
  return (
    <header className="page-header">
      <div className="page-title">
        <h1>Tessera Studio</h1>
        {/* The subtitle describes the view; the page owns it so a tab change cannot leave the previous
            view's sentence behind. */}
        <p className="page-subtitle" id="page-hint">
          {view.hint}
        </p>
      </div>
      <div className="page-meta">
        <ViewTabs current={view.id} onSelect={onSelect} />
        <div className="build-id" id="build-id" ref={buildIdRef} title={buildIdTitle}>
          {buildId}
        </div>
      </div>
    </header>
  );
}

/** One labelled `<select>`; the toolbar has three and they differ only in their options. */
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
 * Region 1's second row: the environment toolbar and the two shared actions.
 *
 * Every view reads it; the Scene and Canvas views apply it, the Diagnostics view carries it into the
 * share link and says so. Keeping one toolbar for three views is the merge's point -- before it, the
 * canvas page had none at all.
 */
export function EnvironmentToolbar({
  controls,
  onChange,
  onToggleGallery,
  galleryOpen,
  onShare,
  shareStatus,
  shareStatusRef,
}: {
  controls: ControlValues;
  onChange: (name: keyof ControlValues, value: string) => void;
  onToggleGallery: () => void;
  galleryOpen: boolean;
  onShare: () => void;
  shareStatus: string;
  shareStatusRef: RefObject<HTMLSpanElement | null>;
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

          It is always present rather than appearing only for a `foldable` skin, because this page
          has no skin picker yet: `designer-studio-v1.md` section 5.2 assigns skins to #171, and a
          control that waits for a skin category nobody can select would be dead chrome. The issue
          requires it "when the skin category is `foldable`"; until #171 lands, the posture IS how a
          visitor chooses a foldable, and the note in docs/design/studio-page.md section 6 records
          that. `None` is the unfolded desktop case, so a visitor who ignores the control sees
          exactly what they saw before this slice.
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
  );
}
