// SPDX-License-Identifier: MIT

// The menu renderer (issue #557): a thin React skin over the pure model. `workbench/menus.js` builds the
// tree, `workbench/menu-nav.js` decides every key, and this file only draws the result with the WAI-ARIA
// menu bar pattern (`menubar` > `menuitem` > `menu` > `menuitem|menuitemcheckbox|menuitemradio`) and moves
// DOM focus to wherever the navigation state says.
//
// `useMenuController` is shared by the menu bar, the device chip's popover and the context menus (#558):
// they differ in where the popup sits, not in how it behaves.

import { useCallback, useEffect, useRef, useState, type KeyboardEvent, type ReactNode } from "react";
import { CLOSED, focusedItem, navigate, type MenuEntry, type MenuNav } from "../workbench/index.js";

type Menus = readonly { id?: string; title?: string; mnemonic?: string; items: MenuEntry[] }[];

export interface MenuController {
  nav: MenuNav;
  setNav: (nav: MenuNav) => void;
  /** Opens menu `bar` with focus on its first item (or last). */
  open: (bar: number, last?: boolean) => void;
  close: () => void;
  /** Handles a key inside the menu surface; returns true when it consumed the event. */
  onKeyDown: (event: KeyboardEvent<HTMLElement>) => void;
  /** The submenu key a pointer is hovering, shown open without moving focus. */
  peek: string | null;
  setPeek: (key: string | null) => void;
  /** Focus-returns: the element that had focus before the menus took it. */
  rememberFocus: (element: Element | null) => void;
  /** Ref callback for the element that contains every popup of this controller. */
  surface: React.RefObject<HTMLElement | null>;
  activate: (entry: MenuEntry) => void;
}

/**
 * The shared behaviour: navigation state, focus placement, Escape and outside-click closing, and running
 * an activated entry. `onRun` receives the entry; `onExit` fires when Escape leaves the bar altogether;
 * `barButton(bar)` finds the element to refocus when a menu closes onto the bar.
 */
export function useMenuController(
  menus: Menus,
  options: { onRun: (entry: MenuEntry) => void; onExit?: () => void; barButton: (bar: number) => HTMLElement | null; onOpenChange?: (open: boolean) => void; barNavigation?: boolean },
): MenuController {
  const [nav, setNavState] = useState<MenuNav>(CLOSED);
  const [peek, setPeek] = useState<string | null>(null);
  const surface = useRef<HTMLElement | null>(null);
  const returnTo = useRef<HTMLElement | null>(null);
  const navRef = useRef(nav);
  navRef.current = nav;
  const { onOpenChange } = options;
  const wasOpen = useRef(false);

  const setNav = useCallback((next: MenuNav) => {
    navRef.current = next;
    setNavState(next);
  }, []);

  const restore = useCallback(() => {
    const target = returnTo.current;
    returnTo.current = null;
    if (target !== null && document.contains(target)) {
      target.focus();
    }
  }, []);

  const close = useCallback(() => {
    setNav({ ...navRef.current, open: false, stack: [] });
    setPeek(null);
  }, [setNav]);

  const rememberFocus = useCallback((element: Element | null) => {
    if (element instanceof HTMLElement && !surface.current?.contains(element)) {
      returnTo.current = element;
    }
  }, []);

  const open = useCallback(
    (bar: number, last = false) => {
      const items = menus[bar]?.items ?? [];
      const order = items.map((item, index) => (item.kind === "separator" ? -1 : index)).filter((index) => index >= 0);
      setNav({ bar, open: true, stack: [last ? (order.at(-1) ?? 0) : (order[0] ?? 0)] });
    },
    [menus, setNav],
  );

  const activate = useCallback(
    (entry: MenuEntry) => {
      if (entry.enabled === false) {
        return;
      }
      close();
      options.onRun(entry);
      restore();
    },
    [close, options, restore],
  );

  const onKeyDown = useCallback(
    (event: KeyboardEvent<HTMLElement>) => {
      if (!navRef.current.open && options.barNavigation === false) {
        return;
      }
      if (event.ctrlKey || event.metaKey || (event.altKey && event.key !== "ArrowDown" && event.key !== "ArrowUp")) {
        return;
      }
      const result = navigate(menus, navRef.current, event.key);
      if (result.handled) {
        event.preventDefault();
        event.stopPropagation();
      }
      setNav(result.nav);
      setPeek(null);
      const effect = result.effect;
      if (effect === null) {
        return;
      }
      if (effect.type === "run") {
        options.onRun(effect.item);
        restore();
      } else if (effect.type === "close-bar") {
        options.barButton(result.nav.bar)?.focus();
      } else if (effect.type === "exit") {
        restore();
        options.onExit?.();
      }
    },
    [menus, options, restore, setNav],
  );

  // Focus follows the navigation state: the item at the end of the stack when a menu is open, the bar
  // button when it just closed onto it (handled in `onKeyDown` for Escape).
  useEffect(() => {
    if (nav.open) {
      const item = focusedItem(menus, nav);
      if (item !== null) {
        surface.current?.querySelector<HTMLElement>(`[data-menu-key="${CSS.escape(item.key)}"]`)?.focus();
      }
    }
    if (wasOpen.current !== nav.open) {
      wasOpen.current = nav.open;
      onOpenChange?.(nav.open);
    }
  }, [nav, menus, onOpenChange]);

  // A press outside every popup closes the menus without stealing the focus the press chose.
  useEffect(() => {
    if (!nav.open) {
      return;
    }
    const onPointerDown = (event: PointerEvent) => {
      if (!surface.current?.contains(event.target as Node)) {
        close();
        returnTo.current = null;
      }
    };
    document.addEventListener("pointerdown", onPointerDown, true);
    return () => document.removeEventListener("pointerdown", onPointerDown, true);
  }, [nav.open, close]);

  return { nav, setNav, open, close, onKeyDown, peek, setPeek, rememberFocus, surface, activate };
}

function glyph(entry: MenuEntry): string {
  if (entry.kind === "check") return entry.checked ? "✓" : "";
  if (entry.kind === "radio") return entry.checked ? "●" : "";
  return "";
}

/**
 * One popup level: a `menu` of items, recursing into the open submenu. `path` is the focused index at each
 * level (`nav.stack`); a level is "open" when the stack reaches it or the pointer peeks at its parent.
 */
export function MenuList({
  items,
  level,
  controller,
  labelledBy,
  id,
}: {
  items: MenuEntry[];
  level: number;
  controller: MenuController;
  labelledBy: string;
  id: string;
}) {
  const { nav, setNav, setPeek, peek, activate } = controller;
  const focused = nav.stack[level];
  return (
    <ul className="menu-popup" role="menu" id={id} aria-labelledby={labelledBy} data-level={level}>
      {items.map((entry, index) => {
        if (entry.kind === "separator") {
          return <li key={entry.key} role="separator" className="menu-separator" />;
        }
        const isSubmenu = entry.kind === "submenu";
        const onPath = nav.stack.length > level + 1 && focused === index;
        const expanded = isSubmenu && (onPath || peek === entry.key);
        const role = entry.kind === "check" ? "menuitemcheckbox" : entry.kind === "radio" ? "menuitemradio" : "menuitem";
        return (
          <li key={entry.key} role="none" className="menu-row">
            <button
              type="button"
              role={role}
              className={`menu-item${entry.enabled ? "" : " is-disabled"}${focused === index && nav.open ? " is-focused" : ""}`}
              data-menu-key={entry.key}
              data-command={entry.command}
              id={`mi-${entry.key.replace(/[^A-Za-z0-9_-]/g, "_")}`}
              tabIndex={-1}
              aria-disabled={entry.enabled ? undefined : true}
              aria-checked={entry.kind === "check" || entry.kind === "radio" ? entry.checked : undefined}
              aria-haspopup={isSubmenu ? "menu" : undefined}
              aria-expanded={isSubmenu ? expanded : undefined}
              aria-keyshortcuts={entry.chord === "" ? undefined : entry.chord}
              title={entry.enabled ? (entry.description || undefined) : entry.reason}
              onMouseEnter={() => {
                setNav({ ...nav, stack: [...nav.stack.slice(0, level), index] });
                setPeek(isSubmenu ? entry.key : null);
              }}
              onClick={() => {
                if (isSubmenu) {
                  const first = entry.items.findIndex((child) => child.kind !== "separator");
                  if (first >= 0) {
                    setNav({ ...nav, open: true, stack: [...nav.stack.slice(0, level), index, first] });
                  }
                } else {
                  activate(entry);
                }
              }}
            >
              <span className="mi-mark" aria-hidden="true">
                {glyph(entry)}
              </span>
              <span className="mi-label">{entry.label}</span>
              {entry.detail ? <span className="mi-detail">{entry.detail}</span> : null}
              {isSubmenu ? (
                <span className="mi-arrow" aria-hidden="true">
                  {"▸"}
                </span>
              ) : entry.chord !== "" ? (
                <span className="mi-chord" aria-hidden="true">
                  {entry.chord}
                </span>
              ) : null}
            </button>
            {expanded ? (
              <MenuList items={entry.items} level={level + 1} controller={controller} labelledBy={`mi-${entry.key.replace(/[^A-Za-z0-9_-]/g, "_")}`} id={`menu-${entry.key.replace(/[^A-Za-z0-9_-]/g, "_")}`} />
            ) : null}
          </li>
        );
      })}
    </ul>
  );
}

/** The application menu bar. At phone width `menus` is one `Menu` entry holding the others as submenus. */
export function MenuBar({
  menus,
  onRun,
  onOpenChange,
  mnemonicRef,
  phone,
}: {
  menus: Menus;
  onRun: (entry: MenuEntry) => void;
  onOpenChange?: (open: boolean) => void;
  /** Receives a function that opens the menu with a given mnemonic letter; returns whether one opened. */
  mnemonicRef?: React.RefObject<((letter: string) => boolean) | null>;
  phone: boolean;
}) {
  const buttons = useRef<(HTMLButtonElement | null)[]>([]);
  const controller = useMenuController(menus, {
    onRun,
    barButton: (bar) => buttons.current[bar] ?? null,
    onOpenChange,
  });
  const { nav, surface } = controller;
  const { open, rememberFocus, setPeek } = controller;

  useEffect(() => {
    if (mnemonicRef === undefined) {
      return;
    }
    mnemonicRef.current = (letter) => {
      const index = menus.findIndex((menu) => menu.mnemonic?.toUpperCase() === letter.toUpperCase());
      if (index < 0) {
        return false;
      }
      rememberFocus(document.activeElement);
      open(index);
      return true;
    };
  }, [menus, mnemonicRef, open, rememberFocus]);

  // When the bar button (not an item) holds focus after a close or a roving move, put DOM focus there.
  useEffect(() => {
    if (!nav.open && surface.current?.contains(document.activeElement)) {
      buttons.current[nav.bar]?.focus();
    }
  }, [nav.bar, nav.open, surface]);

  return (
    <div
      className={`menubar-wrap${phone ? " is-phone" : ""}`}
      ref={surface as React.RefObject<HTMLDivElement>}
      onKeyDown={controller.onKeyDown}
      onFocus={(event) => rememberFocus(event.relatedTarget)}
    >
      <div role="menubar" aria-label="Application menu" className="menubar">
        {menus.map((menu, index) => {
          const isOpen = nav.open && nav.bar === index;
          const buttonId = `menubar-${menu.id}`;
          return (
            <div className="menubar-slot" key={menu.id}>
              <button
                type="button"
                role="menuitem"
                id={buttonId}
                className={`menubar-item${isOpen ? " is-open" : ""}`}
                aria-haspopup="menu"
                aria-expanded={isOpen}
                aria-controls={isOpen ? `menu-${menu.id}` : undefined}
                aria-keyshortcuts={menu.mnemonic ? `Alt+${menu.mnemonic}` : undefined}
                aria-label={phone ? "Menu" : undefined}
                tabIndex={nav.bar === index ? 0 : -1}
                ref={(node) => {
                  buttons.current[index] = node;
                }}
                onClick={() => {
                  if (isOpen) {
                    controller.close();
                  } else {
                    open(index);
                  }
                }}
                onMouseEnter={() => {
                  if (nav.open && nav.bar !== index) {
                    open(index);
                  }
                  setPeek(null);
                }}
              >
                {phone ? <span aria-hidden="true">{"☰"}</span> : <MenuTitle title={menu.title ?? ""} mnemonic={menu.mnemonic ?? ""} />}
              </button>
              {isOpen ? <MenuList items={menu.items} level={0} controller={controller} labelledBy={buttonId} id={`menu-${menu.id}`} /> : null}
            </div>
          );
        })}
      </div>
    </div>
  );
}

/** The title with its mnemonic letter underlined (`aria-hidden`: the shortcut is on the button). */
function MenuTitle({ title, mnemonic }: { title: string; mnemonic: string }): ReactNode {
  const at = mnemonic === "" ? -1 : title.toUpperCase().indexOf(mnemonic.toUpperCase());
  if (at < 0) {
    return title;
  }
  return (
    <>
      {title.slice(0, at)}
      <u>{title[at]}</u>
      {title.slice(at + 1)}
    </>
  );
}
