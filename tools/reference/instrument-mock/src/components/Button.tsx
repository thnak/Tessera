// SPDX-License-Identifier: MIT

// The library Button, as a native <button>. Its look is the shipped packages/tessera-ui/button.css,
// imported unchanged, with the same class names the Tessera Button sets (`tbtn tbtn-<variant>
// tbtn-<size>`, `tbtn-disabled`). button-extra.css turns it into a tactile key (raised gradient,
// lift on hover, sink on press) from the material tokens. `forceState` paints hover/active/focus
// without the pointer so the Controls screen can show every state side by side.
import type { ReactNode } from "react";
import "../../../../../packages/tessera-ui/button.css";
import "./button-extra.css";

// `dark` is the mock's addition (the reference's dark pill key); the rest are the library's variants.
export type ButtonVariant = "primary" | "secondary" | "ghost" | "danger" | "dark";

export type ButtonProps = {
  variant?: ButtonVariant;
  size?: "md" | "sm";
  disabled?: boolean;
  forceState?: "hover" | "active" | "focus";
  type?: "button" | "submit";
  label?: string;
  onClick?: () => void;
  children: ReactNode;
};

export function Button({
  variant = "primary",
  size = "md",
  disabled = false,
  forceState,
  type = "button",
  label,
  onClick,
  children,
}: ButtonProps) {
  const state = disabled ? "tbtn-disabled" : forceState ? `is-${forceState}` : "tbtn-idle";
  return (
    <button
      type={type}
      className={`tbtn tbtn-${variant} tbtn-${size} ${state}`}
      disabled={disabled}
      aria-label={label}
      onClick={onClick}
    >
      {children}
    </button>
  );
}
