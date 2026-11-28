// SPDX-License-Identifier: MIT

// `Radio`, the radio-button behavior primitive (issue #544, ADR-059 group E). It draws nothing: one
// focusable `radio` View whose checked state belongs to the group, so it is a controlled prop, not
// local state. A group is the caller's component: it owns the selected value, renders one `Radio`
// per option with `checked` set from it, and sets `onSelect` to store the choice. Activation
// (pointer, Enter, Space) selects and never unselects, as in a browser. Arrow-key movement among
// the options is the parent's `focusGroup` (docs/design/behavior-hooks.md), which keeps the group
// one Tab stop.

import { Children } from "tessera";

export type RadioProps = {
  label: string;
  checked: boolean;
  onSelect: () => void;
  children: Children;
};

export function Radio({ label, checked, onSelect, children }: RadioProps) {
  return (
    <View
      role="radio"
      tabIndex={0}
      aria-label={label}
      aria-checked={checked ? "true" : "false"}
      className={checked ? "radio checked" : "radio"}
      onClick={() => !checked && onSelect()}
    >
      {children}
    </View>
  );
}
