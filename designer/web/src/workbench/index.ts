// SPDX-License-Identifier: MIT

// The workbench core: the layout model, its operations, migration, reconciliation, the phone projection,
// persistence and the panel host contract. Pure (no DOM, no React, no dock library); the page's dock
// adapter and the command registry both consume this module.

export type * from "./types";
export { LAYOUT_FORMAT, LAYOUT_VERSION } from "./types";
export { PANELS, DEFAULT_LAYOUT, PRESETS, defaultContext, freshDefault, panelDef } from "./context";
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
  splitGroup,
} from "./ops";
export { migrateLayout, migrations, type Migration } from "./migrate";
export { reconcile } from "./reconcile";
export { PHONE_BREAKPOINT_PX, collapse, isPhoneWidth, type PhoneView } from "./phone";
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
  cloneLayout,
  diff,
  findGroup,
  groupOfPanel,
  groupsOf,
  normalize,
  openPanels,
  visiblePanels,
} from "./tree";
export { createPanelRuntime, type Disposable, type PanelHost, type PanelRuntime } from "./panel-host";
export type { DockAdapterHandle, DockAdapterOptions, LayoutCodec } from "./adapter";
