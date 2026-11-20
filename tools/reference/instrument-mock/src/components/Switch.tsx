// SPDX-License-Identifier: MIT

// An on/off switch: a <button role="switch"> with aria-checked, the Toggle primitive's contract.
// Parts: `tswitch` (root row), `tswitch-track`, `tswitch-thumb`, `tswitch-label`, `tswitch-state`
// (a mono ON/OFF readout, so the state is not carried by colour or position alone).
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
        <span className="tswitch-state mono" aria-hidden="true">
          {checked ? "ON" : "OFF"}
        </span>
        <span className="tswitch-track" aria-hidden="true">
          <span className="tswitch-thumb" />
        </span>
      </button>
    </div>
  );
}
