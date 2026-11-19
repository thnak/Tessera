// SPDX-License-Identifier: MIT

// The dock adapter's pure core, bundled for the Node gate (`scripts/ci/workbench_dock_core.mjs`): the
// codec, the gesture map and the splitter arithmetic. No React, no DOM.

export { fromLibrary, pathOfRowId, rowId, toLibrary, visibleLayout } from "./codec";
export { gestureOf, targetOfDrop } from "./actions";
export {
  moveSplitter,
  parseSplitterPath,
  separatorValues,
  sizesForKey,
  splitAtPath,
  ARROW_STEP_PX,
  SHIFT_STEP_PX,
} from "./separators";
