// SPDX-License-Identifier: MIT

// Corpus (spike #660): React.FC with defaults and a template literal class

import React from "react";

interface ButtonProps {
  variant?: "primary" | "secondary";
  disabled?: boolean;
  label: string;
}

export const Button: React.FC<ButtonProps> = ({ variant = "primary", disabled = false, label }) => (
  <button className={`btn btn-${variant}`} disabled={disabled}>
    {label}
  </button>
);
