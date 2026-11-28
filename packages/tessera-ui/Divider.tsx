// SPDX-License-Identifier: MIT

// `Divider`, the hairline rule of the Instrument library (issue #240, stage 1, ADR-047).
// `orientation` is `horizontal` (default, the width of its parent) or `vertical` (a compact control
// height, for a row). Parts: `tdiv`, `tdiv-horizontal` or `tdiv-vertical` from `divider.css`.
//
// Accessibility: purely visual. The engine has no `separator` role, so a Divider is a
// `presentation` box hidden from the accessibility tree; grouping that has meaning belongs in a
// labelled container, not in the rule.

export type DividerProps = { orientation?: string };

export function Divider({ orientation = "horizontal" }: DividerProps) {
  return (
    <View
      role="presentation"
      aria-hidden={true}
      className={`tdiv tdiv-${orientation}`}
    >
    </View>
  );
}
