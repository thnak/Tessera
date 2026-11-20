// SPDX-License-Identifier: MIT

// A radio group: a fieldset with a legend and native radios sharing one name, so the browser gives
// one Tab stop and arrow-key selection (the RovingFocus primitive's contract). Parts: `tradio-group`,
// `tradio-legend`, then per option the checkbox frame with the `tcheck-radio` modifier.
import { useId } from "react";
import "./check.css";

export type RadioOption = { value: string; label: string; disabled?: boolean };

export type RadioGroupProps = {
  legend: string;
  value: string;
  options: RadioOption[];
  onChange: (value: string) => void;
  inline?: boolean;
};

export function RadioGroup({ legend, value, options, onChange, inline = false }: RadioGroupProps) {
  const name = useId();
  return (
    <fieldset className={`tradio-group ${inline ? "tradio-inline" : ""}`}>
      <legend className="tradio-legend">{legend}</legend>
      {options.map((o) => (
        <label key={o.value} className={`tcheck tcheck-radio ${o.disabled ? "tcheck-disabled" : ""}`}>
          <input
            type="radio"
            className="tcheck-input visually-hidden"
            name={name}
            value={o.value}
            checked={value === o.value}
            disabled={o.disabled}
            onChange={() => onChange(o.value)}
          />
          <span className="tcheck-box" aria-hidden="true">
            <span className="tcheck-mark" />
          </span>
          <span className="tcheck-label">{o.label}</span>
        </label>
      ))}
    </fieldset>
  );
}
