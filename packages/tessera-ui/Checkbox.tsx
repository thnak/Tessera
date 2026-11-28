// SPDX-License-Identifier: MIT

// `Checkbox`, the styled boolean choice of the Instrument library (issue #240, stage 1, ADR-047).
// Controlled like `Switch`: the caller owns `checked` and flips it in `onToggle`. `indeterminate`
// shows the mixed state a parent "select all" needs; it wins over `checked` for the accessible state
// and the mark. Parts: `tchk-box`, `tchk-mark` (the library check icon), `tchk-bar` (the mixed
// mark) and `tchk-label`, from `checkbox.css`.
//
// Accessibility: a focusable `checkbox` named by `label` with `aria-checked` "true", "false" or
// "mixed", and `aria-disabled`. A pointer click and Space toggle it (Enter does not, as on the web).

import { Children, KeyEvent } from "tessera";
import { Icon } from "./Icon";

export type CheckboxProps = {
  label: string;
  checked?: boolean;
  indeterminate?: boolean;
  onToggle: () => void;
  disabled?: boolean;
  children: Children;
};

export function Checkbox({
  label,
  checked = false,
  indeterminate = false,
  onToggle,
  disabled = false,
  children,
}: CheckboxProps) {
  return (
    <View
      role="checkbox"
      tabIndex={0}
      aria-label={label}
      aria-checked={indeterminate ? "mixed" : checked ? "true" : "false"}
      aria-disabled={disabled}
      className={`tchk ${indeterminate ? "tchk-mixed" : checked ? "tchk-on" : "tchk-off"} ${disabled ? "tchk-disabled" : "tchk-enabled"}`}
      onKeyDown={(e: KeyEvent) => e.key === " " && !disabled && onToggle()}
      onClick={() => !disabled && onToggle()}
    >
      <View className="tchk-box">
        {checked && !indeterminate && (
          <View className="tchk-mark">
            <Icon name="tabler:check" size="sm" />
          </View>
        )}
        {indeterminate && <View className="tchk-bar"></View>}
      </View>
      <View className="tchk-label">{children}</View>
    </View>
  );
}
