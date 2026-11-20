// SPDX-License-Identifier: MIT

// A labelled single-line text input. Parts: `tfield` (root), `tfield-label` (a micro label),
// `tfield-control` (the recessed slot), `tfield-note` (`tfield-error` when invalid, with an LED).
// The error replaces the hint and is announced through aria-describedby; `aria-invalid` drives the
// danger edge.
import { useId } from "react";
import "./field.css";

export type TextFieldProps = {
  label: string;
  value: string;
  onChange: (value: string) => void;
  hint?: string;
  error?: string;
  placeholder?: string;
  disabled?: boolean;
  required?: boolean;
  mono?: boolean;
  forceFocus?: boolean;
};

export function TextField({
  label,
  value,
  onChange,
  hint,
  error,
  placeholder,
  disabled = false,
  required = false,
  mono = false,
  forceFocus = false,
}: TextFieldProps) {
  const id = useId();
  const noteId = `${id}-note`;
  const note = error || hint;
  return (
    <div className={`tfield ${disabled ? "tfield-disabled" : ""}`}>
      <label className="tfield-label m-micro" htmlFor={id}>
        {label}
        {required ? <span className="tfield-required"> (required)</span> : null}
      </label>
      <input
        id={id}
        className={`tfield-control ${mono ? "mono" : ""} ${forceFocus ? "is-focus" : ""}`}
        value={value}
        placeholder={placeholder}
        disabled={disabled}
        required={required}
        aria-invalid={error ? true : undefined}
        aria-describedby={note ? noteId : undefined}
        onChange={(e) => onChange(e.target.value)}
      />
      {note ? (
        <span id={noteId} className={`tfield-note ${error ? "tfield-error" : ""}`}>
          {error ? <span className="m-led m-led-warn" aria-hidden="true" /> : null}
          {note}
        </span>
      ) : null}
    </div>
  );
}
