// SPDX-License-Identifier: MIT

// The workbench core: the layout model, its operations, migration, reconciliation, the phone projection,
// persistence and the panel host contract. Pure (no DOM, no React, no dock library); the page's dock
// adapter and the command registry both consume this module.

export type * from "./types";
export { LAYOUT_FORMAT, LAYOUT_VERSION } from "./types";
export { PANELS, DEFAULT_LAYOUT, PRESETS, defaultContext, freshDefault, panelDef } from "./layout-context";
export { MAX_DEPTH, MAX_LAYOUT_BYTES, parseLayout, validateLayout } from "./validate";
export {
  activatePanel,
  applyOperation,
  applyPreset,
  closePanel,
  collapseGroup,
  maximize,
  movePanel,
  openPanel,
  resetLayout,
  resizeSplit,
  setShell,
  splitGroup,
} from "./ops";
export {
  COLLAPSE_PX,
  SNAP_PX,
  childMinPx,
  collapsedEdges,
  defaultVisibleSizes,
  dragSash,
  minFractionsOf,
  resetSplit,
  type CollapsedEdge,
} from "./sash";
export { migrateLayout, migrations, type Migration } from "./migrate";
export {
  MAX_PRESET_NAME,
  MAX_USER_PRESETS,
  PRESETS_STORAGE_KEY,
  deleteUserPreset,
  exportLayout,
  importLayout,
  listUserPresets,
  saveUserPreset,
  type ImportResult,
  type SaveResult,
  type UserPreset,
} from "./layout-io";
export { reconcile } from "./reconcile";
export { PHONE_BREAKPOINT_PX, PHONE_GROUP, collapse, isPhoneWidth, phoneLayout, type PhoneView } from "./phone";
export {
  LAYOUT_BACKUP_KEY,
  LAYOUT_STORAGE_KEY,
  SAVE_DEBOUNCE_MS,
  browserStorage,
  createLayoutSaver,
  loadLayout,
  serializeLayout,
  type LayoutSaver,
  type LoadedLayout,
  type StorageLike,
} from "./persist";
export {
  canonicalSizes,
  cloneLayout,
  diff,
  findGroup,
  groupOfPanel,
  groupsOf,
  normalize,
  openPanels,
  visibleLayout,
  visiblePanels,
} from "./tree";
export { createPanelRuntime, type Disposable, type PanelHost, type PanelRuntime } from "./panel-host";
export type { DockAdapterHandle, DockAdapterOptions, LayoutCodec } from "./adapter";
export {
  PANEL_AREA_GROUP,
  directionTarget,
  groupChoices,
  moveMessage,
  type Direction,
  type GroupChoice,
} from "./keyboard";
export {
  createCommands,
  createLayoutStore,
  panelAreaGroup,
  panelAreaOpen,
  sideBarGroup,
  sideBarOpen,
  type LayoutStore,
  type WorkbenchCommands,
} from "./store";
