// SPDX-License-Identifier: MIT

// `RovingFocus`, the roving-focus behavior primitive (issue #236), for a horizontal row such as a
// toolbar or tab list. It draws nothing: one `group` View with `focusGroup="horizontal"`. Its
// focusable descendants (give each `tabIndex={0}`, a role and a name) are a single Tab stop; Left
// and Right move among them and wrap, Home and End jump to the ends, and Tab leaves the group and
// re-enters on the item that was last active. `focusGroup` is a literal, so a column is
// `RovingFocusVertical` rather than a prop.

import { Children } from "tessera";

export type RovingFocusProps = {
  label: string;
  children: Children;
};

export function RovingFocus({ label, children }: RovingFocusProps) {
  return (
    <View role="group" aria-label={label} focusGroup="horizontal">
      {children}
    </View>
  );
}
