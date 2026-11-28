// SPDX-License-Identifier: MIT
//
// Editor groups (#1025): the pure model behind a split editor area.
//
// The workspace keeps one list of open tabs (`open`) and one focused file (`active`). A split adds a second
// group on top of that without a second source of truth: `side` is the subset of `open` shown in the second
// group, and every other open tab is in the main group. Because membership is derived from `open`, a close, a
// delete or a rename that only edits `open` can never leave a tab behind in a group.
//
// `active` is the focused group's file, so everything that already reads "the active file" (the compile, the
// status line, history) keeps working. The group that is not focused remembers its own file in `mainActive` /
// `sideActive`, which `settle` keeps current whenever `active` crosses groups. No buffer lives here: a tab
// moving between groups moves a path, never text, so the dirty set and the canvas are unaffected by design.
//
// The state is plain data in and out (no React, no DOM) so a Node gate exercises every rule.

/** @typedef {{ open: string[], active: string, side: string[], mainActive: string, sideActive: string }} GroupState */

/** The group `path` is in. */
export function groupOf(state, path) {
  return state.side.includes(path) ? 'side' : 'main';
}

/** The tabs of each group, in tab order. `side` is empty when the editor is not split. */
export function groupTabs(state) {
  const side = state.side.filter((path) => state.open.includes(path));
  return { main: state.open.filter((path) => !side.includes(path)), side };
}

/** Whether the editor area is split. */
export function isSplit(state) {
  return groupTabs(state).side.length > 0;
}

/** The file a group shows: the focused file, else the one it remembers, else its last tab, else `""`. */
export function activeOf(state, group) {
  const tabs = groupTabs(state)[group];
  if (tabs.includes(state.active)) return state.active;
  const remembered = group === 'main' ? state.mainActive : state.sideActive;
  if (tabs.includes(remembered)) return remembered;
  return tabs.length > 0 ? tabs[tabs.length - 1] : '';
}

/**
 * Normalizes `next` after any workspace update: drops side tabs that are no longer open, collapses an empty main
 * group into one group, and remembers the file a group was showing when focus moves to the other group.
 */
export function settle(prev, next) {
  if (prev === next) return next;
  let side = [...new Set(next.side.filter((path) => next.open.includes(path)))];
  if (side.length > 0 && next.open.every((path) => side.includes(path))) side = [];
  let { mainActive, sideActive } = next;
  if (prev.active !== next.active && prev.open.includes(prev.active) && next.open.includes(prev.active)) {
    const was = prev.side.includes(prev.active) ? 'side' : 'main';
    const now = side.includes(next.active) ? 'side' : 'main';
    if (was !== now) {
      if (was === 'main') mainActive = prev.active;
      else sideActive = prev.active;
    }
  }
  if (side.length === 0) sideActive = '';
  if (side.length === next.side.length && side.every((path, index) => path === next.side[index]) &&
      mainActive === next.mainActive && sideActive === next.sideActive) {
    return next;
  }
  return { ...next, side, mainActive, sideActive };
}

/**
 * Moves the tab `path` into `to` at `index` (the end when omitted) and focuses it. Reordering inside a group is
 * the same call. A move that would empty the main group collapses the split instead of leaving an empty group.
 */
export function moveTab(state, path, to, index) {
  if (!state.open.includes(path)) return state;
  const tabs = groupTabs(state);
  const main = tabs.main.filter((candidate) => candidate !== path);
  const side = tabs.side.filter((candidate) => candidate !== path);
  const target = to === 'side' ? side : main;
  const at = index === undefined ? target.length : Math.max(0, Math.min(index, target.length));
  target.splice(at, 0, path);
  const open = [...main, ...side];
  return { ...state, open, side: main.length === 0 ? [] : side, active: path };
}

/** Whether Split Editor has something to do: not split yet, and the focused file is not the only tab. */
export function canSplit(state) {
  return !isSplit(state) && state.open.length >= 2 && state.open.includes(state.active);
}

/** Splits the editor area: the focused tab moves to a new second group. */
export function splitActive(state) {
  return canSplit(state) ? moveTab(state, state.active, 'side') : state;
}

/** Merges the second group's tabs back into the main group (order kept); `active` is unchanged. */
export function joinGroups(state) {
  return { ...state, side: [] };
}

/** The file that becomes active when `path` closes: its own group's neighbour, never the other group's tab. */
export function activeAfterClose(state, path) {
  if (state.active !== path) return state.active;
  const group = groupOf(state, path);
  const tabs = groupTabs(state)[group].filter((candidate) => candidate !== path);
  const remembered = group === 'main' ? state.mainActive : state.sideActive;
  if (tabs.includes(remembered)) return remembered;
  if (tabs.length > 0) return tabs[tabs.length - 1];
  const rest = state.open.filter((candidate) => candidate !== path);
  return rest.length > 0 ? rest[rest.length - 1] : '';
}
