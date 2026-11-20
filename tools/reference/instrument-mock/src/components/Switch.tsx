// SPDX-License-Identifier: MIT

// An on/off slide switch: a <button role="switch"> with aria-checked, the Toggle primitive's contract.
// Parts: `tswitch` (root row), `tswitch-label`, `tswitch-control`, `tswitch-state` (LED + mono ON/OFF,
// so the state is never carried by position or colour alone), `tswitch-track` (a dark well),
// `tswitch-thumb` (a raised key with grip lines).
import { useId } from "react";
import "./switch.css";

export type SwitchProps = {
  label: string;
  checked: boolean;
  onChange: (checked: boolean) => void;
  disabled?: boolean;
};

export function Switch({ label, checked, onChange, disabled = false }: SwitchProps) {
  const id = useId();
  return (
    <div className={`tswitch ${disabled ? "tswitch-disabled" : ""}`}>
      <span className="tswitch-label" id={id}>
        {label}
      </span>
      <button
        type="button"
        role="switch"
        aria-checked={checked}
        aria-labelledby={id}
        disabled={disabled}
        className="tswitch-control"
        onClick={() => onChange(!checked)}
      >
        <span className="tswitch-state m-micro" aria-hidden="true">
          <span className={`m-led ${checked && !disabled ? "m-led-on" : ""}`} />
          {checked ? "ON" : "OFF"}
        </span>
        <span className="tswitch-track m-well" aria-hidden="true">
          <span className="tswitch-thumb" />
        </span>
      </button>
    </div>
  );
}
