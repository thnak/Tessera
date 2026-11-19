// SPDX-License-Identifier: MIT

// The studio chrome's shared types. The header, the share and gallery buttons and the preview column's
// environment toolbar that lived here (issues #392, #431) are the workbench now: the title bar, menu bar,
// toolbar and device chip are `regions/WorkbenchChrome.tsx`, driven by the command registry
// (`workbench/commands.json`, `workbench/menus.json`; issues #555 and #557).

/** The toolbar's toggles, as the strings a `<select>` holds. */
export interface ControlValues {
  viewport: string;
  dpr: string;
  theme: string;
  /**
   * The simulated fold posture (issue #408): `""` for no fold, else `flat` | `book` | `tabletop`.
   * A layout input like the viewport, so changing it re-lays the preview out in one pass.
   */
  foldPosture: string;
  /**
   * The simulated device skin (issue #171's skins slice): a directory name under `skins/`, or `""`
   * for "no frame". A layout input like the viewport: the skin's derived viewer size and its insets
   * are what the engine lays out, so changing it re-lays the preview out in one pass.
   */
  skin: string;
  /**
   * The device profile (issue #447): a bundled profile id, `custom` (a size typed into the custom entry), or
   * `""` for the custom size the page always started with. `""` and `custom` are one option in the picker.
   */
  profile: string;
}
