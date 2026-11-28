// SPDX-License-Identifier: MIT

// The workbench-core public surface: pure ES modules, no DOM, no React. The page imports from here.

export { evaluate, parseClause, clauseError, clauseKeys, specificityOf } from "./when.js";
export { CONTEXT_KEYS, createContext, isKnownContextKey } from "./context.js";
export {
  KEYBINDINGS_FORMAT,
  KEYBINDINGS_STORAGE_KEY,
  KEYBINDINGS_VERSION,
  MAX_KEYBINDINGS_BYTES,
  MAX_OVERRIDES,
  chordOfEvent,
  clearBinding,
  collisionsFor,
  keybindingRows,
  loadOverrides,
  resetBinding,
  saveOverrides,
  serializeOverrides,
  setBinding,
  validateOverrides,
} from "./keybindings.js";
export { createKeymap, parseChord, formatChord, chordProblem, reservedProblem, stepOfEvent } from "./keys.js";
export { createRegistry, validateRecords, checkIntegrity } from "./registry.js";
export { validateMenus, walkMenus, buildMenuBar, buildItems, buildContextMenu, buildToolbar } from "./menus.js";
export { CLOSED, navigate, openByMnemonic, focusedItem } from "./menu-nav.js";
export { fuzzyMatch, rankItems, createRecentList, commandItems, parseInput, parseLine, createArgFlow, needsPrompt } from "./palette.js";
export { createNotifications } from "./notifications.js";
