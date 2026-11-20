// SPDX-License-Identifier: MIT

// A checkbox. The native input stays in the tree (visually hidden) for keyboard, form and
// accessibility; the visible parts are `tcheck-box` (with the `tcheck-mark` tick) and
// `tcheck-label`. The same frame draws the radio dot (`tcheck-radio`).
import "./check.css";

export type CheckboxProps = {
  label: string;
  checked: boolean;
  onChange: (checked: boolean) => void;
  disabled?: boolean;
  description?: string;
};

export function Checkbox({ label, checked, onChange, disabled = false, description }: CheckboxProps) {
  return (
    <label className={`tcheck ${disabled ? "tcheck-disabled" : ""}`}>
      <input
        type="checkbox"
        className="tcheck-input visually-hidden"
        checked={checked}
        disabled={disabled}
        onChange={(e) => onChange(e.target.checked)}
      />
      <span className="tcheck-box" aria-hidden="true">
        <span className="tcheck-mark" />
      </span>
      <span className="tcheck-text">
        <span className="tcheck-label">{label}</span>
        {description ? <span className="tcheck-description">{description}</span> : null}
      </span>
    </label>
  );
}
