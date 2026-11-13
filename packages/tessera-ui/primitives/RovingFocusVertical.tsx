// SPDX-License-Identifier: MIT

// `RovingFocusVertical`, the roving-focus primitive for a column such as a menu or list (issue
// #236). Same contract as `RovingFocus`, with Up and Down as the arrows.

import { Children } from "tessera";

export type RovingFocusVerticalProps = {
  label: string;
  children: Children;
};

export function RovingFocusVertical({ label, children }: RovingFocusVerticalProps) {
  return (
    <View role="group" aria-label={label} focusGroup="vertical">
      {children}
    </View>
  );
}
