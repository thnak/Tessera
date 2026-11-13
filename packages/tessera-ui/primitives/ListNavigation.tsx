// SPDX-License-Identifier: MIT

// `ListNavigation`, the list-navigation and typeahead behavior primitive (issue #236). It draws
// nothing: one `listbox` View with `focusGroup="vertical"` and `typeahead`. Its focusable items
// (`tabIndex={0}`, role `option`, an `aria-label`) are one Tab stop; Up and Down move among them and
// wrap, Home and End jump to the ends, and typing characters moves to the next item whose label
// starts with them (the prefix resets after a second; a repeated character cycles through the items
// that start with it). Selection is the composer's: put `aria-selected` and an `onClick` on each
// item. `typeahead` also works on any other `focusGroup`.
//
// Grid navigation (two axes with row and column structure) is not here yet: `focusGroup="both"`
// moves linearly through the items, which is right for a wrapping list, not for a grid.

import { Children } from "tessera";

export type ListNavigationProps = {
  label: string;
  children: Children;
};

export function ListNavigation({ label, children }: ListNavigationProps) {
  return (
    <View role="listbox" aria-label={label} focusGroup="vertical" typeahead>
      {children}
    </View>
  );
}
