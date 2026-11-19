// SPDX-License-Identifier: MIT

// Context menus (issue #558): `tree.item`, `editor`, `panel.tab` from `menus.json`, opened by a right click or
// by `Shift+F10` / the Menu key on the focused element. The items are the same command entries the menu bar
// builds, so a context menu cannot do anything the registry does not; the popup is the shared `MenuList` with
// the same keyboard model. When a context has no live items (the dock's `panel.tab` entries are reserved until
// the dock lands) nothing is opened and the browser's own menu shows.

import { useEffect, useRef, useState } from "react";
import type { MenuEntry } from "../workbench/index.js";
import type { Workbench } from "../page-commands";
import { MenuList, useMenuController } from "./MenuBar";

interface Open {
  name: string;
  x: number;
  y: number;
  items: MenuEntry[];
  returnTo: Element | null;
  nonce: number;
}

/** Which context menu applies to an element, or `null`. */
export function contextNameOf(target: Element | null): string | null {
  if (target === null) {
    return null;
  }
  if (target.closest(".cm-editor") !== null) {
    return "editor";
  }
  if (target.closest('[role="treeitem"], #project-tree') !== null) {
    return "tree.item";
  }
  if (target.closest('[role="tab"]') !== null) {
    return "panel.tab";
  }
  return null;
}

function ContextPopup({ workbench, open, onClose }: { workbench: Workbench; open: Open; onClose: () => void }) {
  const menus = [{ id: open.name, title: open.name, mnemonic: "", items: open.items }];
  const controller = useMenuController(menus, {
    onRun: (entry) => void workbench.activate(entry),
    barButton: () => null,
    barNavigation: false,
    onExit: onClose,
    onOpenChange: (isOpen) => {
      workbench.setFocusOwner(isOpen ? "menu" : null);
      if (!isOpen) {
        onClose();
      }
    },
  });
  const { open: openMenu, rememberFocus } = controller;
  useEffect(() => {
    rememberFocus(open.returnTo);
    openMenu(0);
    // Once per mount; the key (nonce) remounts for the next menu.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);
  // The popup is fixed at the pointer; keep it inside the viewport.
  const style = { left: Math.max(4, Math.min(open.x, window.innerWidth - 260)), top: Math.max(4, Math.min(open.y, window.innerHeight - 40)) };
  return (
    <div className="context-menu" style={style} ref={controller.surface as React.RefObject<HTMLDivElement>} onKeyDown={controller.onKeyDown}>
      {controller.nav.open ? <MenuList items={open.items} level={0} controller={controller} labelledBy="context-menu-label" id={`context-${open.name}`} /> : null}
      <span id="context-menu-label" className="visually-hidden">
        {open.name === "tree.item" ? "File actions" : open.name === "editor" ? "Editor actions" : "Panel actions"}
      </span>
    </div>
  );
}

export function ContextMenuHost({ workbench }: { workbench: Workbench }) {
  const [open, setOpen] = useState<Open | null>(null);
  const nonce = useRef(0);
  const workbenchRef = useRef(workbench);
  workbenchRef.current = workbench;

  useEffect(() => {
    function show(target: Element | null, x: number, y: number): boolean {
      const name = contextNameOf(target);
      if (name === null) {
        return false;
      }
      const items = workbenchRef.current.contextItems(name);
      if (items.every((item) => item.kind === "separator")) {
        return false;
      }
      if (name === "tree.item") {
        // A right click selects the row the menu acts on (Rename and Delete work on the selection).
        const row = target?.closest<HTMLElement>('[role="treeitem"]');
        if (row !== null && row !== undefined && row.getAttribute("aria-selected") !== "true") {
          row.click();
        }
      }
      nonce.current += 1;
      setOpen({ name, x, y, items, returnTo: document.activeElement, nonce: nonce.current });
      return true;
    }
    const onContextMenu = (event: MouseEvent) => {
      if (event.defaultPrevented) {
        return;
      }
      const keyboard = event.clientX === 0 && event.clientY === 0 && event.detail === 0;
      const focused = document.activeElement;
      const target = keyboard ? focused : (event.target as Element | null);
      const box = (keyboard ? focused : null)?.getBoundingClientRect();
      const x = box ? box.left + 24 : event.clientX;
      const y = box ? box.top + Math.min(box.height, 28) : event.clientY;
      if (show(target, x, y)) {
        event.preventDefault();
      }
    };
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.defaultPrevented || !(event.key === "ContextMenu" || (event.shiftKey && event.key === "F10"))) {
        return;
      }
      const focused = document.activeElement;
      const box = focused?.getBoundingClientRect();
      if (focused !== null && box !== undefined && show(focused, box.left + 24, box.top + Math.min(box.height, 28))) {
        event.preventDefault();
      }
    };
    document.addEventListener("contextmenu", onContextMenu);
    window.addEventListener("keydown", onKeyDown);
    return () => {
      document.removeEventListener("contextmenu", onContextMenu);
      window.removeEventListener("keydown", onKeyDown);
    };
  }, []);

  return open === null ? null : <ContextPopup key={open.nonce} workbench={workbench} open={open} onClose={() => setOpen(null)} />;
}
