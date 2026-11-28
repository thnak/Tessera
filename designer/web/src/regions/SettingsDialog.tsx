// SPDX-License-Identifier: MIT

// The settings dialog (slice W8 #565): `view.openSettings` (Ctrl+,) and `view.openKeybindings`. Three tabs: keyboard shortcuts
// (rebind, conflicts, JSON), layout (import, export, saved presets) and appearance (the color theme).
//
// An accessible modal like About: `role="dialog"` with `aria-modal`, focus moves in and returns, Tab cycles inside it,
// Escape leaves. The tabs follow the ARIA tab pattern (roving tabindex, arrow keys). Every setting is also a command or
// a JSON file, so the dialog is a view over them, not the only way in.

import { useEffect, useRef, useState } from "react";
import type { Workbench } from "../page-commands";
import type { SettingsTab, Outcome } from "../page-settings";
import { JsonTransfer } from "./JsonTransfer";
import { KeybindingsPane } from "./KeybindingsPane";

const TABS: { id: SettingsTab; label: string }[] = [
  { id: "keybindings", label: "Keyboard shortcuts" },
  { id: "layout", label: "Layout" },
  { id: "appearance", label: "Appearance" },
];

const THEME_CHOICES = [
  { value: "system", label: "System" },
  { value: "light", label: "Light" },
  { value: "dark", label: "Dark" },
];

function LayoutPane({ workbench, onChanged }: { workbench: Workbench; onChanged: () => void }) {
  const tools = workbench.settings.layout;
  const [name, setName] = useState("");
  const [outcome, setOutcome] = useState<Outcome | null>(null);
  const presets = tools.presets();
  const report = (result: Outcome) => {
    setOutcome(result);
    onChanged();
  };
  return (
    <div className="settings-pane" id="settings-layout">
      <p className="settings-note">Your layout is stored in this browser only. Built-in presets are in View, Layout Preset.</p>
      <h3 className="settings-heading">Saved layouts</h3>
      <div className="settings-row">
        <input
          type="text"
          id="layout-preset-name"
          className="kb-filter"
          aria-label="Name for the current layout"
          placeholder="Name for the current layout"
          maxLength={40}
          value={name}
          onChange={(event) => setName(event.target.value)}
        />
        <button
          type="button"
          id="layout-preset-save"
          disabled={name.trim() === ""}
          onClick={() => {
            const result = tools.savePreset(name);
            if (result.ok) {
              setName("");
            }
            report(result);
          }}
        >
          Save current layout
        </button>
      </div>
      {presets.length === 0 ? (
        <p className="settings-note" id="layout-preset-empty">
          No saved layouts yet.
        </p>
      ) : (
        <ul className="preset-list" id="layout-presets" aria-label="Saved layouts">
          {presets.map((preset) => (
            <li key={preset.name} data-preset={preset.name}>
              <span className="preset-name">{preset.name}</span>
              <button type="button" aria-label={`Apply layout ${preset.name}`} onClick={() => report(tools.applyPreset(preset.name))}>
                Apply
              </button>
              <button
                type="button"
                aria-label={`Delete layout ${preset.name}`}
                onClick={() => {
                  tools.deletePreset(preset.name);
                  report({ ok: true, message: `Deleted '${preset.name}'.` });
                }}
              >
                Delete
              </button>
            </li>
          ))}
        </ul>
      )}
      <p className={outcome !== null && !outcome.ok ? "settings-outcome is-error" : "settings-outcome"} id="layout-preset-outcome" role="status">
        {outcome?.message ?? ""}
      </p>
      <h3 className="settings-heading">Import or export</h3>
      <JsonTransfer kind="layout" fileName="tessera-layout.json" exportText={tools.exportText} onImport={tools.importText} onChanged={onChanged} />
    </div>
  );
}

function AppearancePane({ workbench }: { workbench: Workbench }) {
  const current = String(workbench.registry.context.get("ui.theme"));
  return (
    <div className="settings-pane" id="settings-appearance">
      <fieldset className="settings-fieldset">
        <legend>Color theme</legend>
        {THEME_CHOICES.map((choice) => (
          <label key={choice.value} className="settings-radio">
            <input type="radio" name="theme" value={choice.value} checked={current === choice.value} onChange={() => void workbench.run("view.setTheme", { value: choice.value })} />
            {choice.label}
          </label>
        ))}
      </fieldset>
    </div>
  );
}

export function SettingsDialog({ workbench, tab, onTab, onClose }: { workbench: Workbench; tab: SettingsTab; onTab: (tab: SettingsTab) => void; onClose: () => void }) {
  const dialog = useRef<HTMLDivElement>(null);
  const returnTo = useRef<Element | null>(null);
  const [, bump] = useState(0);
  const refresh = () => bump((value) => value + 1);

  useEffect(() => {
    returnTo.current = document.activeElement;
    dialog.current?.querySelector<HTMLElement>('[role="tab"][aria-selected="true"]')?.focus();
    return () => {
      if (returnTo.current instanceof HTMLElement && document.contains(returnTo.current)) {
        returnTo.current.focus();
      }
    };
  }, []);

  // Escape closes the dialog even when focus has just left it (a control that unmounts or is disabled drops focus to the
  // body). Fields that use Escape themselves (the shortcut editor) stop the event first, so this only sees the rest.
  useEffect(() => {
    const onWindowKey = (event: KeyboardEvent) => {
      if (event.key === "Escape" && !event.defaultPrevented) {
        event.preventDefault();
        onClose();
      }
    };
    window.addEventListener("keydown", onWindowKey);
    return () => window.removeEventListener("keydown", onWindowKey);
  }, [onClose]);

  function onKeyDown(event: React.KeyboardEvent<HTMLDivElement>) {
    if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      onClose();
    } else if (event.key === "Tab") {
      const stops = Array.from(dialog.current?.querySelectorAll<HTMLElement>("a[href], button:not([disabled]), input:not([disabled]):not([hidden]), textarea, select, [role='tab'][tabindex='0']") ?? []);
      const first = stops[0];
      const last = stops[stops.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last?.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first?.focus();
      }
    }
  }

  function onTabKey(event: React.KeyboardEvent<HTMLButtonElement>, index: number) {
    const step = event.key === "ArrowRight" ? 1 : event.key === "ArrowLeft" ? -1 : 0;
    if (step === 0 && event.key !== "Home" && event.key !== "End") {
      return;
    }
    event.preventDefault();
    const next = event.key === "Home" ? 0 : event.key === "End" ? TABS.length - 1 : (index + step + TABS.length) % TABS.length;
    onTab(TABS[next].id);
    requestAnimationFrame(() => dialog.current?.querySelector<HTMLElement>(`#settings-tab-${TABS[next].id}`)?.focus());
  }

  return (
    <div className="about-layer" onMouseDown={(event) => event.target === event.currentTarget && onClose()}>
      <div className="about settings" id="settings-dialog" role="dialog" aria-modal="true" aria-labelledby="settings-title" ref={dialog} onKeyDown={onKeyDown}>
        <h2 id="settings-title" className="about-title">
          Settings
        </h2>
        <div role="tablist" aria-label="Settings sections" className="settings-tabs">
          {TABS.map((entry, index) => (
            <button
              key={entry.id}
              type="button"
              role="tab"
              id={`settings-tab-${entry.id}`}
              aria-selected={tab === entry.id}
              aria-controls={`settings-panel-${entry.id}`}
              tabIndex={tab === entry.id ? 0 : -1}
              onClick={() => onTab(entry.id)}
              onKeyDown={(event) => onTabKey(event, index)}
            >
              {entry.label}
            </button>
          ))}
        </div>
        <div role="tabpanel" id={`settings-panel-${tab}`} aria-labelledby={`settings-tab-${tab}`}>
          {tab === "keybindings" ? <KeybindingsPane tools={workbench.settings.keybindings} onChanged={refresh} /> : null}
          {tab === "layout" ? <LayoutPane workbench={workbench} onChanged={refresh} /> : null}
          {tab === "appearance" ? <AppearancePane workbench={workbench} /> : null}
        </div>
        <div className="about-actions">
          <button type="button" className="primary" id="settings-close" onClick={onClose}>
            Close
          </button>
        </div>
      </div>
    </div>
  );
}
