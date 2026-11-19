// SPDX-License-Identifier: MIT

// The studio's top chrome (issue #557): the menu bar, the toolbar and the device chip on one row, with the
// palette button at the right (the title and description are Help > About; the bell is in the status bar).
// It replaces the page header and the preview column's five `<select>`s; every control here is a projection of
// a command (`page-commands.ts` registers the handlers, `workbench/menus.json` says what appears).
//
// The ids the page's older code and the developer-run browser scripts look up are kept where a control has a
// direct successor: `restart-app` (Restart), `save-project` (Save). What the old status line and the project note said is a
// notification now (`regions/Notifications.tsx`). The Samples gallery, Open folder/zip, Download, the device `<select>`s and the
// component picker are menu items now and have no element ids of their own.

import { useEffect, useRef, useState, useSyncExternalStore, type KeyboardEvent } from "react";
import type { MenuEntry, ToolbarEntry } from "../workbench/index.js";
import type { Workbench } from "../page-commands";
import { AboutDialog } from "./AboutDialog";
import { CommandPalette } from "./CommandPalette";
import { ContextMenuHost } from "./ContextMenuHost";
import { MenuBar, MenuList, useMenuController } from "./MenuBar";
import { ToastStack } from "./Notifications";

const TOOLBAR_IDS: Record<string, string> = {
  "run.restart": "restart-app",
  "file.save": "save-project",
  "view.cycleTheme": "cycle-theme",
};

const ICONS: Record<string, string> = {
  "run.restart": "▶",
  "file.save": "💾︎",
  "view.cycleTheme": "◐",
};

/** The device chip: one button showing the device and opening the Device menu as a popover. */
function DeviceChip({ workbench, entries, tabIndex }: { workbench: Workbench; entries: MenuEntry[]; tabIndex: number }) {
  const button = useRef<HTMLButtonElement>(null);
  const menus = [{ id: "device-chip", title: "Device", mnemonic: "", items: entries }];
  const controller = useMenuController(menus, {
    onRun: (entry) => void workbench.activate(entry),
    barButton: () => button.current,
    barNavigation: false,
  });
  const { nav, surface } = controller;
  const isOpen = nav.open;
  return (
    <div className="device-chip" ref={surface as React.RefObject<HTMLDivElement>} onKeyDown={controller.onKeyDown} onFocus={(event) => controller.rememberFocus(event.relatedTarget)}>
      <button
        type="button"
        id="device-chip"
        ref={button}
        data-roving=""
        tabIndex={tabIndex}
        className="tb-button chip"
        aria-haspopup="menu"
        aria-expanded={isOpen}
        aria-controls={isOpen ? "menu-device-chip" : undefined}
        aria-label={`Device: ${workbench.chipText}`}
        title="Device profile, skin, viewport, pixel ratio and fold"
        onClick={() => (isOpen ? controller.close() : controller.open(0))}
        onKeyDown={(event) => {
          if (!isOpen && (event.key === "ArrowDown" || event.key === "Enter" || event.key === " ")) {
            event.preventDefault();
            event.stopPropagation();
            controller.open(0);
          } else if (!isOpen && event.key === "ArrowUp") {
            event.preventDefault();
            event.stopPropagation();
            controller.open(0, true);
          }
        }}
      >
        <span className="chip-text">{workbench.chipText}</span>
        <span aria-hidden="true" className="chip-caret">
          {"▾"}
        </span>
      </button>
      {isOpen ? <MenuList items={entries} level={0} controller={controller} labelledBy="device-chip" id="menu-device-chip" /> : null}
    </div>
  );
}

function ToolbarButton({ entry, workbench, tabIndex }: { entry: ToolbarEntry; workbench: Workbench; tabIndex: number }) {
  const command = entry.command ?? "";
  const label = entry.label ?? command;
  const dirty = command === "file.save" ? workbench.dirtyCount : 0;
  const tip = [entry.enabled === false ? `${label} (${entry.reason})` : (entry.description || label), entry.chord ? `(${entry.chord})` : ""].filter(Boolean).join(" ");
  return (
    <button
      type="button"
      id={TOOLBAR_IDS[command]}
      className={`tb-button${entry.emphasis === "primary" ? " primary" : ""}`}
      data-command={command}
      disabled={false}
      aria-disabled={entry.enabled === false ? true : undefined}
      aria-label={dirty > 0 ? `${label}, ${dirty} unsaved file${dirty === 1 ? "" : "s"}` : label}
      aria-pressed={typeof entry.checked === "boolean" ? entry.checked : undefined}
      aria-keyshortcuts={entry.chord || undefined}
      title={tip}
      tabIndex={tabIndex}
      data-roving=""
      onClick={() => {
        if (entry.enabled !== false) {
          void workbench.activate(entry);
        }
      }}
    >
      <span aria-hidden="true" className="tb-icon">
        {ICONS[command] ?? ""}
      </span>
      <span className="tb-label">{label}</span>
      {dirty > 0 ? (
        <span className="tb-badge" aria-hidden="true">
          {dirty}
        </span>
      ) : null}
    </button>
  );
}

/** The toolbar: one tab stop, arrow keys move inside it (roving tabindex), like `HardwareStrip`. */
function Toolbar({ workbench }: { workbench: Workbench }) {
  const ref = useRef<HTMLDivElement>(null);
  const [active, setActive] = useState(0);
  const device = workbench.allMenus.find((menu) => menu.id === "device");
  const entries = workbench.toolbar.filter((entry) => entry.widget !== "paletteButton");

  function move(event: KeyboardEvent<HTMLDivElement>) {
    if (event.target instanceof HTMLElement && event.target.closest(".menu-popup")) {
      return;
    }
    const nodes = Array.from(ref.current?.querySelectorAll<HTMLElement>("[data-roving]") ?? []);
    const index = nodes.indexOf(document.activeElement as HTMLElement);
    if (index < 0) {
      return;
    }
    let next = -1;
    if (event.key === "ArrowRight") next = (index + 1) % nodes.length;
    else if (event.key === "ArrowLeft") next = (index - 1 + nodes.length) % nodes.length;
    else if (event.key === "Home") next = 0;
    else if (event.key === "End") next = nodes.length - 1;
    if (next >= 0) {
      event.preventDefault();
      setActive(next);
      nodes[next].focus();
    }
  }

  return (
    <div className="toolbar-row" role="toolbar" aria-label="Studio actions" aria-orientation="horizontal" ref={ref} onKeyDown={move}>
      {entries.map((entry, position) => (
        <span className="tb-widget" key={entry.key} data-secondary={entry.command === "run.restart" ? undefined : ""}>
          {entry.widget === "deviceChip" ? (
            <DeviceChip workbench={workbench} entries={device?.items ?? []} tabIndex={position === active ? 0 : -1} />
          ) : (
            <ToolbarButton entry={entry} workbench={workbench} tabIndex={position === active ? 0 : -1} />
          )}
        </span>
      ))}
    </div>
  );
}

/** The whole top chrome. */
export function WorkbenchChrome({ workbench, zen = false }: { workbench: Workbench; zen?: boolean }) {
  useSyncExternalStore(workbench.subscribe, workbench.version, workbench.version);
  const mnemonic = useRef<((letter: string) => boolean) | null>(null);
  const paletteVisible = workbench.registry.describe("view.palette")?.visible === true;
  useEffect(() => workbench.onMnemonic((letter) => mnemonic.current?.(letter) ?? false), [workbench]);
  return (
    <header className={zen ? "workbench-chrome is-zen" : "workbench-chrome"} aria-label="Studio">
      <div className="titlebar">
        {/* The page's one h1 stays for assistive technology; the visible title and description are Help > About. */}
        <h1 className="visually-hidden">Tessera Studio</h1>
        <MenuBar
          menus={workbench.menus}
          phone={workbench.phone}
          mnemonicRef={mnemonic}
          onRun={(entry) => void workbench.activate(entry)}
          onOpenChange={(open) => workbench.setFocusOwner(open ? "menu" : null)}
        />
        <Toolbar workbench={workbench} />
        <span className="titlebar-spacer" />
        {paletteVisible ? (
          <button type="button" id="palette-button" className="palette-button" onClick={() => void workbench.run("view.palette")} aria-keyshortcuts="Control+Shift+P">
            <span aria-hidden="true">{"⌕"}</span> Search or run a command
          </button>
        ) : null}
      </div>
      <input
        ref={workbench.zipInputRef}
        type="file"
        accept=".zip,application/zip"
        hidden
        aria-label="Project zip"
        onChange={(event) => {
          const file = event.currentTarget.files?.[0];
          event.currentTarget.value = "";
          if (file) {
            workbench.openZip(file);
          }
        }}
      />
      {workbench.aboutOpen ? <AboutDialog buildId={workbench.buildId} onClose={() => workbench.setAboutOpen(false)} /> : null}
      {workbench.palette.request !== null ? <CommandPalette key={workbench.palette.request.nonce} workbench={workbench} request={workbench.palette.request} /> : null}
      <ContextMenuHost workbench={workbench} />
      <ToastStack workbench={workbench} />
    </header>
  );
}
