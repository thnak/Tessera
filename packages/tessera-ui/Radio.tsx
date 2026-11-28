// SPDX-License-Identifier: MIT

// `Radio`, the styled single choice of the Instrument library (issue #240, stage 1, ADR-047).
// `RadioGroup` (RadioGroup.tsx) is the labelled container; each `Radio` is controlled: the caller
// keeps the selected value and passes `selected` to the matching Radio, whose `onSelect` writes the
// new value. Parts: `trg`, `trad-dot`, `trad-mark` and `trad-label`, from `radio.css`.
//
// Accessibility: a Radio is a focusable `radio` named by `label` with `aria-checked` and
// `aria-disabled`; a click and Space select it. Selecting an already selected Radio calls
// `onSelect` again (idempotent for a caller that stores the value). Arrow keys rove inside the
// enclosing `RadioGroup` (a `focusGroup`, one Tab stop, the runtime skips `aria-disabled` radios
// and selects the one an arrow lands on); Space is a synthesized keyboard click (issue #1035).

import { Children } from "tessera";

export type RadioProps = {
  label: string;
  selected?: boolean;
  onSelect: () => void;
  disabled?: boolean;
  children: Children;
};

export function Radio({
  label,
  selected = false,
  onSelect,
  disabled = false,
  children,
}: RadioProps) {
  return (
    <View
      role="radio"
      tabIndex={0}
      aria-label={label}
      aria-checked={selected ? "true" : "false"}
      aria-disabled={disabled}
      className={`trad ${selected ? "trad-on" : "trad-off"} ${disabled ? "trad-disabled" : "trad-enabled"}`}
      onClick={() => !disabled && onSelect()}
    >
      <View className="trad-dot">{selected && <View className="trad-mark"></View>}</View>
      <View className="trad-label">{children}</View>
    </View>
  );
}
