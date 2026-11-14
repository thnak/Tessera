// SPDX-License-Identifier: MIT

// `Button`, the styled action control of the Instrument library (issue #239, ADR-047). It is the
// `Press` behavior (focusable `button`, `pressed` class, Enter/Space/pointer) with the library look
// from `button.css`: variants `primary`, `secondary`, `ghost` and `danger`, sizes `md` and `sm`,
// and a disabled state. The look uses theme tokens only, so every theme and density restyles it.
//
// A disabled Button never calls `onPress` and never shows as pressed; it keeps its accessible name
// and reports `aria-disabled`. A pointer press styles the Button through `:active` (only Click
// bubbles through generated bindings, so the label under the pointer would hide `onPointerDown`);
// a key press sets the `tbtn-pressed` class.

import { Children, KeyEvent, useState } from "tessera";

export type ButtonProps = {
  label: string;
  onPress: () => void;
  variant?: string;
  size?: string;
  disabled?: boolean;
  children: Children;
};

export function Button({
  label,
  onPress,
  variant = "primary",
  size = "md",
  disabled = false,
  children,
}: ButtonProps) {
  const [pressed, setPressed] = useState<boolean>(false);
  return (
    <View
      role="button"
      tabIndex={0}
      aria-label={label}
      aria-disabled={disabled}
      className={`tbtn tbtn-${variant} tbtn-${size} ${disabled ? "tbtn-disabled" : pressed ? "tbtn-pressed" : "tbtn-idle"}`}
      onPointerDown={() => !disabled && setPressed(true)}
      onPointerUp={() => setPressed(false)}
      onPointerLeave={() => setPressed(false)}
      onKeyDown={(e: KeyEvent) => (e.key === "Enter" || e.key === " ") && !disabled && setPressed(true)}
      onKeyUp={() => setPressed(false)}
      onClick={() => !disabled && onPress()}
    >
      {children}
    </View>
  );
}
