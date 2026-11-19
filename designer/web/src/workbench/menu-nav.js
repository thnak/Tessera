// SPDX-License-Identifier: MIT

// Keyboard model of the menu bar (WAI-ARIA APG "menu bar" pattern), as a pure reducer so the Node gate can
// drive it and a Tessera view can reuse it. State is `{ bar, open, stack }`: the roving menu-bar index,
// whether a menu is open, and the focused item index at each open level (`stack[0]` in the menu,
// `stack[1]` in its open submenu, ...). A disabled item is focusable and does nothing when activated.
//
// `navigate(menus, nav, key)` returns `{ nav, handled, effect }`; `effect` is one of
//   { type: 'run', item }   activate a command item (the renderer closes the menus and runs it)
//   { type: 'exit' }        Escape on the bar: return focus to whoever had it
//   { type: 'close-bar' }   Escape at the root of an open menu: close it, focus the bar button
//   { type: 'close' }       Tab: close without moving focus
// A renderer reads `nav` to place DOM focus: the menu-bar button `bar`, or the item at `stack`.

export const CLOSED = Object.freeze({ bar: 0, open: false, stack: Object.freeze([]) });

function itemsAt(menus, nav, level) {
  let items = menus[nav.bar]?.items ?? [];
  for (let depth = 0; depth < level; depth += 1) {
    items = items[nav.stack[depth]]?.items ?? [];
  }
  return items;
}

const focusable = (items) => items.map((item, index) => (item.kind === "separator" ? -1 : index)).filter((index) => index >= 0);

function step(items, current, delta) {
  const list = focusable(items);
  if (list.length === 0) {
    return -1;
  }
  const at = list.indexOf(current);
  return list[at < 0 ? (delta > 0 ? 0 : list.length - 1) : (at + delta + list.length) % list.length];
}

function openMenu(menus, bar, last = false) {
  const index = (bar + menus.length) % menus.length;
  const items = menus[index]?.items ?? [];
  const list = focusable(items);
  const first = list.length === 0 ? 0 : last ? list[list.length - 1] : list[0];
  return { bar: index, open: true, stack: [first] };
}

/** Opens a top-level menu from its Alt+letter mnemonic; `null` when no menu has that letter. */
export function openByMnemonic(menus, letter) {
  const index = menus.findIndex((menu) => menu.mnemonic?.toUpperCase() === letter.toUpperCase());
  return index < 0 ? null : openMenu(menus, index);
}

/** Feeds one `KeyboardEvent.key`. Printable single characters type ahead within the focused level. */
export function navigate(menus, nav, key) {
  const stay = { nav, handled: false, effect: null };
  if (menus.length === 0) {
    return stay;
  }
  if (!nav.open) {
    switch (key) {
      case "ArrowRight":
        return { nav: { ...nav, bar: (nav.bar + 1) % menus.length }, handled: true, effect: null };
      case "ArrowLeft":
        return { nav: { ...nav, bar: (nav.bar - 1 + menus.length) % menus.length }, handled: true, effect: null };
      case "Home":
        return { nav: { ...nav, bar: 0 }, handled: true, effect: null };
      case "End":
        return { nav: { ...nav, bar: menus.length - 1 }, handled: true, effect: null };
      case "ArrowDown":
      case "Enter":
      case " ":
        return { nav: openMenu(menus, nav.bar), handled: true, effect: null };
      case "ArrowUp":
        return { nav: openMenu(menus, nav.bar, true), handled: true, effect: null };
      case "Escape":
        return { nav, handled: true, effect: { type: "exit" } };
      default:
        return stay;
    }
  }
  const level = nav.stack.length - 1;
  const items = itemsAt(menus, nav, level);
  const current = nav.stack[level];
  const item = items[current];
  const withStack = (stack) => ({ ...nav, stack });
  const replaceTop = (index) => withStack([...nav.stack.slice(0, level), index]);
  switch (key) {
    case "ArrowDown":
      return { nav: replaceTop(step(items, current, 1)), handled: true, effect: null };
    case "ArrowUp":
      return { nav: replaceTop(step(items, current, -1)), handled: true, effect: null };
    case "Home":
      return { nav: replaceTop(focusable(items)[0] ?? current), handled: true, effect: null };
    case "End":
      return { nav: replaceTop(focusable(items).at(-1) ?? current), handled: true, effect: null };
    case "ArrowRight": {
      if (item?.kind === "submenu") {
        const first = focusable(item.items)[0];
        return first === undefined ? { nav, handled: true, effect: null } : { nav: withStack([...nav.stack, first]), handled: true, effect: null };
      }
      return { nav: openMenu(menus, nav.bar + 1), handled: true, effect: null };
    }
    case "ArrowLeft":
      return level > 0
        ? { nav: withStack(nav.stack.slice(0, -1)), handled: true, effect: null }
        : { nav: openMenu(menus, nav.bar - 1), handled: true, effect: null };
    case "Enter":
    case " ": {
      if (item === undefined) {
        return { nav, handled: true, effect: null };
      }
      if (item.kind === "submenu") {
        const first = focusable(item.items)[0];
        return first === undefined ? { nav, handled: true, effect: null } : { nav: withStack([...nav.stack, first]), handled: true, effect: null };
      }
      if (item.enabled === false) {
        return { nav, handled: true, effect: null };
      }
      return { nav: { ...nav, open: false, stack: [] }, handled: true, effect: { type: "run", item } };
    }
    case "Escape":
      return level > 0
        ? { nav: withStack(nav.stack.slice(0, -1)), handled: true, effect: null }
        : { nav: { ...nav, open: false, stack: [] }, handled: true, effect: { type: "close-bar" } };
    case "Tab":
      return { nav: { ...nav, open: false, stack: [] }, handled: false, effect: { type: "close" } };
    default: {
      if (key.length === 1 && /\S/.test(key)) {
        const letter = key.toLowerCase();
        const list = focusable(items);
        const at = list.indexOf(current);
        const ordered = [...list.slice(at + 1), ...list.slice(0, at + 1)];
        const hit = ordered.find((index) => items[index].label.toLowerCase().startsWith(letter));
        return hit === undefined ? { nav, handled: true, effect: null } : { nav: replaceTop(hit), handled: true, effect: null };
      }
      return stay;
    }
  }
}

/** The item at the end of the focus stack (what DOM focus belongs on), or `null` when no menu is open. */
export function focusedItem(menus, nav) {
  if (!nav.open) {
    return null;
  }
  return itemsAt(menus, nav, nav.stack.length - 1)[nav.stack.at(-1)] ?? null;
}
