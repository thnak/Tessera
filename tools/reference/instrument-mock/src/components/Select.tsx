// SPDX-License-Identifier: MIT

// A labelled native <select>. Parts: `tfield` (root), `tfield-label`, `tselect` (control frame),
// `tfield-control`, `tselect-chevron`, `tfield-hint`. Native keeps the platform list, typeahead and
// accessibility; the Tessera port would compose ListNavigation in a dropdown layer instead.
import { useId } from "react";
import "./field.css";

export type SelectOption = { value: string; label: string };

export type SelectProps = {
  label: string;
  value: string;
  options: SelectOption[];
  onChange: (value: string) => void;
  hint?: string;
  disabled?: boolean;
  compact?: boolean;
  hideLabel?: boolean;
};

export function Select({
  label,
  value,
  options,
  onChange,
  hint,
  disabled = false,
  compact = false,
  hideLabel = false,
}: SelectProps) {
  const id = useId();
  return (
    <div className={`tfield ${disabled ? "tfield-disabled" : ""}`}>
      <label className={hideLabel ? "visually-hidden" : "tfield-label"} htmlFor={id}>
        {label}
      </label>
      <div className={`tselect ${compact ? "tselect-compact" : ""}`}>
        <select
          id={id}
          className="tfield-control"
          value={value}
          disabled={disabled}
          aria-describedby={hint ? `${id}-hint` : undefined}
          onChange={(e) => onChange(e.target.value)}
        >
          {options.map((o) => (
            <option key={o.value} value={o.value}>
              {o.label}
            </option>
          ))}
        </select>
        <span className="tselect-chevron" aria-hidden="true" />
      </div>
      {hint ? (
        <span id={`${id}-hint`} className="tfield-hint">
          {hint}
        </span>
      ) : null}
    </div>
  );
}
