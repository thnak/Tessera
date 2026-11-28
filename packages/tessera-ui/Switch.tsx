// SPDX-License-Identifier: MIT

// `Switch`, the styled on/off control of the Instrument library (issue #240, stage 1, ADR-047). It
// is controlled: the caller owns `checked` and flips it in `onToggle`, because a callback prop
// carries no payload today (the caller reads its own state). The look is `switch.css`: a track and
// a thumb, parts `tsw-track`, `tsw-thumb` and `tsw-label`, theme tokens only.
//
// Accessibility: a focusable `switch` named by `label` that reports `aria-checked` ("true" or
// "false") and `aria-disabled`. A pointer click, Space and Enter toggle it; the runtime synthesizes
// keyboard clicks only for `button`, so the key handler is explicit. A disabled Switch never calls
// `onToggle`.

import { Children, KeyEvent } from "tessera";

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
      onKeyDown={(e: KeyEvent) => (e.key === " " || e.key === "Enter") && !disabled && onToggle()}
      onClick={() => !disabled && onToggle()}
    >
      <View className="tsw-track"><View className="tsw-thumb"></View></View>
      <View className="tsw-label">{children}</View>
    </View>
  );
}
