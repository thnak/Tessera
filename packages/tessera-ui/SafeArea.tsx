// SPDX-License-Identifier: MIT

// `SafeArea`, the library layout wrapper that keeps its children out of notches, system bars, the
// keyboard and a hinge (issue #223, docs/specs/safe-area-v1.md). It draws nothing and has no role:
// the wrapper is ignored by the semantics tree.
//
// The component only declares what it wants through class tokens (`safe-edges-top+bottom`,
// `safe-sources-ime`, `safe-min-8`, `safe-mode-margin`); the JSX subset has no custom-property
// `style` keys. The host owns the numbers: before layout it reads the window metrics, resolves the
// insets for these edges and sources, and writes them as the node's `padding-*` (`mode="padding"`,
// the background stays under the unsafe area) or `margin-*` (`mode="margin"`, it does not). The
// keyboard and rotation animate per edge in the host; reduced motion jumps.
//
//   edges    any of `top right bottom left start end`, joined with `+` (start/end follow direction)
//   sources  any of `system-bars cutout ime gestures hinge`, joined with `+`; `gestures` is off by
//            default because it is large on gesture navigation
//   minimum  a floor per applying side, in whole px, so a device with no insets still keeps a margin
//   mode     `padding` or `margin`

import { Children } from "tessera";

export type SafeAreaProps = {
  edges?: string;
  sources?: string;
  minimum?: int;
  mode?: string;
  children: Children;
};

export function SafeArea({
  edges = "top+right+bottom+left",
  sources = "system-bars+cutout+ime+hinge",
  minimum = 0,
  mode = "padding",
  children,
}: SafeAreaProps) {
  return (
    <View
      className={`tsafe safe-edges-${edges} safe-sources-${sources} safe-min-${minimum} safe-mode-${mode}`}
    >
      {children}
    </View>
  );
}
