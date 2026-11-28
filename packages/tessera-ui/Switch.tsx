// SPDX-License-Identifier: MIT

// `Switch`, the styled on/off control of the Instrument library (issue #240, stage 1, ADR-047). It
// is controlled: the caller owns `checked` and flips it in `onToggle`, because a callback prop
// carries no payload today (the caller reads its own state). The look is `switch.css`: a track and
// a thumb, parts `tsw-track`, `tsw-thumb` and `tsw-label`, theme tokens only.
//
// Accessibility: a focusable `switch` named by `label` that reports `aria-checked` ("true" or
// "false") and `aria-disabled`. A pointer click, Space and Enter toggle it: the runtime
// synthesizes the keyboard click for a focusable `switch` (issue #1035). A disabled Switch never
// calls `onToggle`.

import { Children } from "tessera";

export type SwitchProps = {
  label: string;
  checked?: boolean;
  onToggle: () => void;
  disabled?: boolean;
  children: Children;
};

export function Switch({
  label,
  checked = false,
  onToggle,
  disabled = false,
  children,
}: SwitchProps) {
  return (
    <View
      role="switch"
      tabIndex={0}
      aria-label={label}
      aria-checked={checked ? "true" : "false"}
      aria-disabled={disabled}
      className={`tsw ${checked ? "tsw-on" : "tsw-off"} ${disabled ? "tsw-disabled" : "tsw-enabled"}`}
      onClick={() => !disabled && onToggle()}
    >
      <View className="tsw-track"><View className="tsw-thumb"></View></View>
      <View className="tsw-label">{children}</View>
    </View>
  );
}
